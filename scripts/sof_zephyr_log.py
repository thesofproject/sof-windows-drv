#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 The SOF Authors
#
# Sound Open Firmware (SOF) - Windows Zephyr Logging & Trace Tooling Suite
#
# Supports all SOF FW Zephyr logging configurations on Windows:
#   1. ADSP Mtrace SRAM Debug Window (CONFIG_LOG_BACKEND_ADSP_MTRACE)
#      - Plain Text output (CONFIG_LOG_BACKEND_ADSP_MTRACE_OUTPUT_TEXT)
#      - Dictionary output (CONFIG_LOG_BACKEND_ADSP_MTRACE_OUTPUT_DICTIONARY)
#      - MIPI Sys-T output (CONFIG_LOG_MIPI_SYST_ENABLE)
#   2. SOF Probes DMA Logging (CONFIG_LOG_BACKEND_SOF_PROBE)
#   3. Classic Winstream ADSP Console (CONFIG_LOG_BACKEND_ADSP)
#   4. Post-mortem crash coredumps (sof_dsp_coredump.bin)
#
# Pure-Python implementation: runs natively on Windows with standard library only.
# No Linux-specific dependencies, no select() socket errors on Windows.

import argparse
import binascii
import json
import os
import re
import struct
import sys
import time
from typing import Any, Dict, List, Optional, Tuple, Union

# ==============================================================================
# 1. PURE-PYTHON ELF READER (No external dependencies on Windows)
# ==============================================================================

class ZephyrElfReader:
    """
    Lightweight, pure-Python 32-bit and 64-bit ELF parser for Zephyr firmware binaries.
    Extracts string tables, rodata, log constants, symbols, and build IDs on Windows.
    """
    EI_NIDENT = 16

    def __init__(self, filepath: str):
        self.filepath = filepath
        self.is_64bit = False
        self.is_little_endian = True
        self.endian_prefix = "<"
        self.sections: Dict[str, Dict[str, Any]] = {}
        self.symbols: Dict[str, int] = {}
        self.build_id: Optional[str] = None
        self._parse()

    def _parse(self):
        if not os.path.exists(self.filepath):
            raise FileNotFoundError(f"ELF file not found: {self.filepath}")

        with open(self.filepath, "rb") as f:
            ident = f.read(self.EI_NIDENT)
            if len(ident) < self.EI_NIDENT or ident[:4] != b"\x7fELF":
                raise ValueError(f"Invalid ELF magic in {self.filepath}")

            self.is_64bit = (ident[4] == 2)
            self.is_little_endian = (ident[5] == 1)
            self.endian_prefix = "<" if self.is_little_endian else ">"
            e = self.endian_prefix

            if self.is_64bit:
                f.seek(0)
                hdr = f.read(64)
                # ELF64 header
                e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, \
                e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = \
                    struct.unpack(f"{e}HHIQQQIHHHHHH", hdr[16:64])
            else:
                f.seek(0)
                hdr = f.read(52)
                # ELF32 header
                e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, \
                e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = \
                    struct.unpack(f"{e}HHIIIIIHHHHHH", hdr[16:52])

            # Read section header string table
            f.seek(e_shoff + e_shstrndx * e_shentsize)
            shstr_hdr = f.read(e_shentsize)
            if self.is_64bit:
                sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = \
                    struct.unpack(f"{e}IIQQQQ", shstr_hdr[:40])
            else:
                sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = \
                    struct.unpack(f"{e}IIIIII", shstr_hdr[:24])

            f.seek(sh_offset)
            shstrtab = f.read(sh_size)

            # Read all section headers
            for i in range(e_shnum):
                f.seek(e_shoff + i * e_shentsize)
                sh_raw = f.read(e_shentsize)
                if self.is_64bit:
                    s_name, s_type, s_flags, s_addr, s_offset, s_size, s_link, s_info, s_addralign, s_entsize = \
                        struct.unpack(f"{e}IIQQQQIIQQ", sh_raw[:64])
                else:
                    s_name, s_type, s_flags, s_addr, s_offset, s_size, s_link, s_info, s_addralign, s_entsize = \
                        struct.unpack(f"{e}IIIIIIIIII", sh_raw[:40])

                name = shstrtab[s_name:].split(b"\x00")[0].decode("latin1", errors="replace")
                f.seek(s_offset)
                data = f.read(s_size)

                self.sections[name] = {
                    "type": s_type,
                    "flags": s_flags,
                    "addr": s_addr,
                    "offset": s_offset,
                    "size": s_size,
                    "link": s_link,
                    "info": s_info,
                    "entsize": s_entsize,
                    "data": data,
                }

            # Parse symbol table if present
            if ".symtab" in self.sections and ".strtab" in self.sections:
                sym_data = self.sections[".symtab"]["data"]
                str_data = self.sections[".strtab"]["data"]
                entsize = self.sections[".symtab"]["entsize"]
                if entsize == 0:
                    entsize = 24 if self.is_64bit else 16

                count = len(sym_data) // entsize
                for i in range(count):
                    s_chunk = sym_data[i * entsize : (i + 1) * entsize]
                    if self.is_64bit:
                        st_name, st_info, st_other, st_shndx, st_value, st_size = \
                            struct.unpack(f"{e}IBBHQQ", s_chunk)
                    else:
                        st_name, st_value, st_size, st_info, st_other, st_shndx = \
                            struct.unpack(f"{e}IIIBBH", s_chunk)

                    if st_name < len(str_data):
                        sym_name = str_data[st_name:].split(b"\x00")[0].decode("latin1", errors="replace")
                        if sym_name:
                            self.symbols[sym_name] = st_value

            # Extract GNU Build ID if present
            for note_sec in [".note.gnu.build-id", ".notes"]:
                if note_sec in self.sections:
                    ndata = self.sections[note_sec]["data"]
                    if len(ndata) >= 16:
                        # namesz(4), descsz(4), type(4)
                        namesz, descsz, ntype = struct.unpack(f"{e}III", ndata[:12])
                        if ntype == 3: # NT_GNU_BUILD_ID
                            name_pad = (namesz + 3) & ~3
                            desc = ndata[12 + name_pad : 12 + name_pad + descsz]
                            self.build_id = desc.hex()
                            break

    def get_string_at_va(self, vaddr: int) -> Optional[str]:
        """Looks up a null-terminated string at the specified virtual address in any loaded section."""
        for sec_name, sec in self.sections.items():
            base = sec["addr"]
            size = sec["size"]
            if base <= vaddr < base + size:
                offset = vaddr - base
                raw_chunk = sec["data"][offset : offset + 512]
                null_pos = raw_chunk.find(b"\x00")
                if null_pos != -1:
                    raw_chunk = raw_chunk[:null_pos]
                return raw_chunk.decode("utf-8", errors="replace")
        return None

    def extract_log_database(self) -> Dict[str, Any]:
        """
        Extracts Zephyr Dictionary Database JSON dictionary directly from ELF.
        Compatible with Zephyr's dictionary_parser database format.
        """
        e = self.endian_prefix
        database: Dict[str, Any] = {
            "version": 3,
            "target": {
                "bits": 64 if self.is_64bit else 32,
                "little_endian": self.is_little_endian,
            },
            "build_id": self.build_id or "unknown",
            "arch": "xtensa" if not self.is_64bit else "x86_64",
            "kconfigs": {
                "CONFIG_LOG": "y",
                "CONFIG_LOG_DICTIONARY_SUPPORT": "y",
                "CONFIG_LOG_TIMESTAMP_64BIT": "y",
            },
            "log_subsys": {
                "log_instances": {},
                "log_sources": {},
            },
            "strings": {},
        }

        # 1. Extract strings from .rodata and log_strings
        for sec_name in [".rodata", "rodata", "log_strings", ".log_strings"]:
            if sec_name in self.sections:
                sec = self.sections[sec_name]
                base = sec["addr"]
                data = sec["data"]
                # Walk through null-terminated strings
                start = 0
                for idx, b in enumerate(data):
                    if b == 0:
                        if idx > start:
                            s = data[start:idx].decode("latin1", errors="replace")
                            # Keep readable strings
                            if len(s) > 0 and all(31 < ord(c) < 127 or c in "\r\n\t" for c in s):
                                addr = base + start
                                database["strings"][f"0x{addr:08x}"] = s
                        start = idx + 1

        # 2. Extract log_const_area entries (log source metadata)
        if "log_const_area" in self.sections:
            lsec = self.sections["log_const_area"]
            ldata = lsec["data"]
            ptr_size = 8 if self.is_64bit else 4
            fmt_ptr = f"{e}Q" if self.is_64bit else f"{e}I"
            # Each entry in log_const_area is:
            #   const char *name;
            #   uint8_t level; (padded to ptr_size or 4)
            step = ptr_size * 2
            entries = len(ldata) // step
            for i in range(entries):
                chunk = ldata[i * step : (i + 1) * step]
                name_ptr = struct.unpack(fmt_ptr, chunk[:ptr_size])[0]
                source_name = self.get_string_at_va(name_ptr) or f"src_0x{name_ptr:08x}"
                vaddr = lsec["addr"] + i * step
                database["log_subsys"]["log_sources"][f"0x{vaddr:08x}"] = {
                    "name": source_name,
                    "index": i,
                }

        return database


# ==============================================================================
# 2. ZEPHYR LOG DATABASE LOADER
# ==============================================================================

class ZephyrLogDatabase:
    """Represents a loaded Zephyr dictionary logging database."""
    def __init__(self, db_dict: Dict[str, Any]):
        self.data = db_dict
        self.version = db_dict.get("version", 3)
        target = db_dict.get("target", {})
        self.is_64bit = (target.get("bits", 32) == 64)
        self.is_little_endian = target.get("little_endian", True)
        self.build_id = db_dict.get("build_id", "unknown")
        self.arch = db_dict.get("arch", "xtensa")
        self.kconfigs = db_dict.get("kconfigs", {})
        self.strings = db_dict.get("strings", {})
        self.log_sources = db_dict.get("log_subsys", {}).get("log_sources", {})

    @classmethod
    def from_json_file(cls, filepath: str) -> "ZephyrLogDatabase":
        with open(filepath, "r", encoding="utf-8") as f:
            return cls(json.load(f))

    @classmethod
    def from_elf_file(cls, filepath: str) -> "ZephyrLogDatabase":
        reader = ZephyrElfReader(filepath)
        return cls(reader.extract_log_database())

    def find_string(self, addr: int) -> Optional[str]:
        # Try both 32-bit hex and 64-bit hex
        k32 = f"0x{addr:08x}"
        if k32 in self.strings:
            return self.strings[k32]
        k64 = f"0x{addr:016x}"
        if k64 in self.strings:
            return self.strings[k64]
        return None

    def get_source_name(self, source_id: int) -> str:
        k32 = f"0x{source_id:08x}"
        if k32 in self.log_sources:
            return self.log_sources[k32].get("name", k32)
        k64 = f"0x{source_id:016x}"
        if k64 in self.log_sources:
            return self.log_sources[k64].get("name", k64)
        # Fallback to string table lookup
        s = self.find_string(source_id)
        if s:
            return s
        return f"src_{source_id:x}"


# ==============================================================================
# 3. ZEPHYR DICTIONARY DECODER (v1, v2, v3)
# ==============================================================================

class ZephyrDictionaryDecoder:
    """
    Decodes Zephyr dictionary-based binary log streams (versions 1, 2, and 3).
    Expands cbvprintf packages with full argument unpacking and format string resolution.
    """
    LOG_LEVELS = {
        0: ("NONE", "\033[90m"),
        1: ("ERR",  "\033[91m"), # Red
        2: ("WRN",  "\033[93m"), # Yellow
        3: ("INF",  "\033[92m"), # Green
        4: ("DBG",  "\033[96m"), # Cyan
    }

    def __init__(self, database: Optional[ZephyrLogDatabase] = None):
        self.database = database
        self.endian = "<" if (database is None or database.is_little_endian) else ">"
        self.is_64bit = False if database is None else database.is_64bit

    def decode_stream(self, data: bytes) -> List[Dict[str, Any]]:
        """Parses a contiguous stream of binary dictionary log packets."""
        results = []
        offset = 0
        total_len = len(data)

        while offset < total_len:
            if offset + 1 > total_len:
                break

            msg_type = struct.unpack_from(f"{self.endian}B", data, offset)[0]
            offset += 1

            if msg_type == 1:
                # MSG_TYPE_DROPPED: dropped count (2 bytes)
                if offset + 2 > total_len:
                    break
                cnt = struct.unpack_from(f"{self.endian}H", data, offset)[0]
                offset += 2
                results.append({
                    "type": "dropped",
                    "count": cnt,
                    "message": f"<--- Dropped {cnt} messages --->",
                })
                continue
            elif msg_type == 0:
                # MSG_TYPE_NORMAL
                # v3 Header layout:
                #   uint8_t domain_lvl; (domain:4, level:4)
                #   uint16_t pkg_len;
                #   uint16_t data_len;
                #   uintptr_t source;
                #   log_timestamp_t timestamp; (32-bit or 64-bit)
                ptr_fmt = "Q" if self.is_64bit else "I"
                ptr_size = 8 if self.is_64bit else 4
                ts_fmt = "Q" # standard 64-bit timestamp in modern Zephyr / SOF
                ts_size = 8

                hdr_fmt = f"{self.endian}BHH{ptr_fmt}{ts_fmt}"
                hdr_size = struct.calcsize(hdr_fmt)

                if offset + hdr_size > total_len:
                    # Check if 32-bit timestamp
                    hdr_fmt32 = f"{self.endian}BHH{ptr_fmt}I"
                    hdr_size32 = struct.calcsize(hdr_fmt32)
                    if offset + hdr_size32 > total_len:
                        break
                    domain_lvl, pkg_len, data_len, source_id, timestamp = \
                        struct.unpack_from(hdr_fmt32, data, offset)
                    offset += hdr_size32
                else:
                    domain_lvl, pkg_len, data_len, source_id, timestamp = \
                        struct.unpack_from(hdr_fmt, data, offset)
                    offset += hdr_size

                level = (domain_lvl >> 4) & 0x0F
                domain = domain_lvl & 0x0F

                if offset + pkg_len + data_len > total_len:
                    # Partial / truncated package at end of buffer
                    break

                pkg_data = data[offset : offset + pkg_len]
                raw_data = data[offset + pkg_len : offset + pkg_len + data_len]
                offset += pkg_len + data_len

                # Unpack cbvprintf package
                formatted_msg = self._unpack_package(pkg_data)

                source_name = "sof"
                if self.database:
                    source_name = self.database.get_source_name(source_id)

                lvl_str, color = self.LOG_LEVELS.get(level, (f"L{level}", ""))

                entry = {
                    "type": "normal",
                    "level": lvl_str,
                    "level_num": level,
                    "domain": domain,
                    "source": source_name,
                    "source_id": source_id,
                    "timestamp": timestamp,
                    "message": formatted_msg,
                    "raw_data": raw_data,
                    "color": color,
                }
                results.append(entry)
            else:
                # Unrecognized or out-of-sync message type, advance by 1 byte
                offset += 1

        return results

    def _unpack_package(self, pkg: bytes) -> str:
        """Decodes a Zephyr cbvprintf package into a formatted string."""
        if len(pkg) < 4:
            return "<empty package>"

        ptr_size = 8 if self.is_64bit else 4
        fmt_ptr = f"{self.endian}Q" if self.is_64bit else f"{self.endian}I"

        # First word in cbvprintf package is pointer to format string
        fmt_str_addr = struct.unpack_from(fmt_ptr, pkg, 0)[0]
        fmt_str = None
        if self.database:
            fmt_str = self.database.find_string(fmt_str_addr)

        if not fmt_str:
            fmt_str = f"<fmt_0x{fmt_str_addr:x}>"
            return fmt_str

        # Subsequent bytes contain arguments
        # Let's extract format arguments based on % conversion specifiers
        args = []
        pos = ptr_size
        regex = re.compile(r"%([0-9]*\.?[0-9]*)?(ll|l|z|j|t|h)?([diuoxXspcfaAeEgG%])")
        matches = regex.findall(fmt_str)

        for width_prec, modifier, spec in matches:
            if spec == "%":
                continue
            if pos >= len(pkg):
                args.append("?")
                continue

            if spec in ("d", "i"):
                if modifier == "ll":
                    val = struct.unpack_from(f"{self.endian}q", pkg, pos)[0]
                    pos += 8
                else:
                    val = struct.unpack_from(f"{self.endian}i", pkg, pos)[0]
                    pos += 4
                args.append(val)
            elif spec in ("u", "x", "X", "o"):
                if modifier == "ll":
                    val = struct.unpack_from(f"{self.endian}Q", pkg, pos)[0]
                    pos += 8
                else:
                    val = struct.unpack_from(f"{self.endian}I", pkg, pos)[0]
                    pos += 4
                args.append(val)
            elif spec in ("p", "s"):
                ptr = struct.unpack_from(fmt_ptr, pkg, pos)[0]
                pos += ptr_size
                if spec == "s" and self.database:
                    s_val = self.database.find_string(ptr) or f"str_0x{ptr:x}"
                    args.append(s_val)
                else:
                    args.append(f"0x{ptr:x}")
            elif spec in ("f", "a", "e", "g"):
                val = struct.unpack_from(f"{self.endian}f", pkg, pos)[0]
                pos += 4
                args.append(val)
            elif spec == "c":
                ch = struct.unpack_from(f"{self.endian}B", pkg, pos)[0]
                pos += 4
                args.append(chr(ch))

        # Format string interpolation
        try:
            # Convert C format string syntax to Python syntax where needed
            py_fmt = re.sub(r"%(ll|z|j|t)", "%", fmt_str)
            formatted = py_fmt % tuple(args)
        except Exception:
            formatted = f"{fmt_str} [Args: {args}]"

        return formatted.strip("\r\n")


# ==============================================================================
# 4. ZEPHYR TEXT LOG DECODER
# ==============================================================================

class ZephyrTextDecoder:
    """
    Parses and colorizes raw Zephyr text logs (ASCII/UTF-8 streams from
    CONFIG_LOG_BACKEND_ADSP_MTRACE_OUTPUT_TEXT or driver trace ringbuffers).
    """
    LINE_RE = re.compile(
        r"^(?:<(?P<lvl>err|wrn|inf|dbg|none)>)?\s*"
        r"(?:\[(?P<ts>[0-9\.:,\s]+)\])?\s*"
        r"(?:\((?:Core\s*(?P<core>\d+))\))?\s*"
        r"(?:<(?P<mod>[a-zA-Z0-9_\-\.]+)>:)?\s*"
        r"(?P<msg>.*)$",
        re.IGNORECASE
    )

    LEVEL_COLORS = {
        "err": "\033[91m", # Red
        "wrn": "\033[93m", # Yellow
        "inf": "\033[92m", # Green
        "dbg": "\033[96m", # Cyan
        "none": "\033[90m", # Gray
    }

    def decode_text(self, text: str) -> List[Dict[str, Any]]:
        """Parses multi-line text output into structured records."""
        lines = text.splitlines()
        records = []
        for line in lines:
            line_str = line.strip()
            if not line_str:
                continue

            m = self.LINE_RE.match(line_str)
            if m and (m.group("lvl") or m.group("ts") or m.group("core") or m.group("mod")):
                lvl = (m.group("lvl") or "INF").upper()
                ts = (m.group("ts") or "").strip()
                core = m.group("core")
                core_id = int(core) if core else 0
                mod = m.group("mod") or "sof"
                msg = m.group("msg") or line_str
                color = self.LEVEL_COLORS.get(lvl.lower(), "")
                records.append({
                    "type": "text",
                    "level": lvl,
                    "timestamp": ts,
                    "core": core_id,
                    "module": mod,
                    "message": msg,
                    "raw": line_str,
                    "color": color,
                })
            else:
                records.append({
                    "type": "text",
                    "level": "INF",
                    "timestamp": "",
                    "core": 0,
                    "module": "raw",
                    "message": line_str,
                    "raw": line_str,
                    "color": "",
                })
        return records


# ==============================================================================
# 5. MIPI SYS-T DECODER
# ==============================================================================

class MipiSystDecoder:
    """
    Decodes MIPI System Trace (Sys-T) protocol frames generated by
    CONFIG_LOG_MIPI_SYST_ENABLE.
    """
    def decode_frames(self, data: bytes) -> List[Dict[str, Any]]:
        results = []
        pos = 0
        total = len(data)

        while pos + 4 <= total:
            # 32-bit header
            header = struct.unpack_from("<I", data, pos)[0]
            # Sys-T framing:
            # bits [3:0]: Message Type (0=Origin, 1=Catalog, 2=Short, 3=Payload, etc.)
            # bits [7:4]: Severity (1=Fatal, 2=Error, 3=Warning, 4=Info, 5=Debug)
            # bits [15:8]: Payload Length
            msg_type = header & 0x0F
            severity = (header >> 4) & 0x0F
            payload_len = (header >> 8) & 0xFF
            pos += 4

            sev_names = {1: "FATAL", 2: "ERR", 3: "WRN", 4: "INF", 5: "DBG"}
            sev_str = sev_names.get(severity, "INF")

            if pos + payload_len > total:
                break

            payload = data[pos : pos + payload_len]
            pos += payload_len

            if msg_type == 1 and len(payload) >= 4: # Catalog message
                cat_id = struct.unpack_from("<I", payload, 0)[0]
                results.append({
                    "type": "syst_catalog",
                    "severity": sev_str,
                    "catalog_id": f"0x{cat_id:08x}",
                    "message": f"[Sys-T Catalog 0x{cat_id:08x}]",
                })
            elif msg_type == 2 and len(payload) >= 4: # Short message
                sval = struct.unpack_from("<I", payload, 0)[0]
                results.append({
                    "type": "syst_short",
                    "severity": sev_str,
                    "value": sval,
                    "message": f"[Sys-T Short 0x{sval:08x}]",
                })
            else:
                text = payload.decode("latin1", errors="replace").strip("\r\n\x00")
                results.append({
                    "type": "syst_data",
                    "severity": sev_str,
                    "message": text or f"[Sys-T Type {msg_type} Len {payload_len}]",
                })

        return results


# ==============================================================================
# 6. SOF PROBE STREAM DECODER
# ==============================================================================

class SofProbeStreamDecoder:
    """
    Unpacks SOF Probes packets (PROBE_PURPOSE_LOGGING) multiplexed over DMA.
    """
    PROBE_PURPOSE_LOGGING = 1
    PROBE_HEADER_SIZE = 8

    def extract_log_packets(self, stream_data: bytes) -> bytes:
        """Extracts log payload bytes from demultiplexed probe streaming data."""
        out = bytearray()
        pos = 0
        total = len(stream_data)

        while pos + self.PROBE_HEADER_SIZE <= total:
            # Probe packet header:
            #   uint32_t sync_word_and_size; (sync=0x534F4650 'SOFP', or size in low 16)
            #   uint16_t stream_tag;
            #   uint16_t purpose;
            w0, stream_tag, purpose = struct.unpack_from("<IHH", stream_data, pos)
            pkt_size = w0 & 0xFFFF
            pos += self.PROBE_HEADER_SIZE

            if pkt_size > 0 and pos + pkt_size <= total:
                payload = stream_data[pos : pos + pkt_size]
                pos += pkt_size
                if purpose == self.PROBE_PURPOSE_LOGGING or purpose == 0:
                    out.extend(payload)
            else:
                # Direct passthrough if not probe framed
                out.extend(stream_data[pos - self.PROBE_HEADER_SIZE:])
                break

        return bytes(out)


# ==============================================================================
# 7. ADSP SRAM DEBUG WINDOW UNPACKER
# ==============================================================================

class SramDebugWindowUnpacker:
    """
    Unpacks ADSP SRAM Debug Window slots (Intel cAVS / ACE microarchitectures).
    Layout:
      Page 0 (0..4095): 16 Slot Descriptors (12 bytes: res_id, type, vma)
        SOF_IPC4_DEBUG_SLOT_DEBUG_LOG    = 0x474f4c00 | core
        SOF_IPC4_DEBUG_SLOT_CRITICAL_LOG = 0x54524300 | core
      Page 1..15 (4096 bytes each):
        uint32_t host_read_ptr;
        uint32_t dsp_write_ptr;
        uint8_t buffer[4088]; (circular)
    """
    PAGE_SIZE = 4096
    SLOT_DESC_SIZE = 12
    MAX_SLOTS = 15
    DEBUG_LOG_MAGIC = 0x474f4c00 # "LOG\0"
    CRITICAL_LOG_MAGIC = 0x54524300 # "TRC\0"

    def unpack_window(self, win_data: bytes) -> Dict[int, bytes]:
        """
        Extracts unread circular log data for each core mapped in the debug window.
        Returns a dict mapping core_id -> unread bytes.
        """
        core_buffers: Dict[int, bytes] = {}
        if len(win_data) < self.PAGE_SIZE * 2:
            # Standalone slot buffer (no page 0 descriptors)
            # Treat entire buffer as a single slot (Core 0)
            if len(win_data) >= 8:
                core_buffers[0] = self.unpack_single_slot(win_data)
            return core_buffers

        # Parse Page 0 descriptors
        page0 = win_data[: self.PAGE_SIZE]
        for i in range(self.MAX_SLOTS):
            desc_offset = i * self.SLOT_DESC_SIZE
            res_id, stype, vma = struct.unpack_from("<III", page0, desc_offset)
            magic = stype & 0xFFFFFF00
            core = stype & 0x000000FF

            if magic in (self.DEBUG_LOG_MAGIC, self.CRITICAL_LOG_MAGIC):
                slot_page_offset = (i + 1) * self.PAGE_SIZE
                if slot_page_offset + self.PAGE_SIZE <= len(win_data):
                    slot_bytes = win_data[slot_page_offset : slot_page_offset + self.PAGE_SIZE]
                    core_buffers[core] = self.unpack_single_slot(slot_bytes)

        return core_buffers

    def unpack_single_slot(self, slot_bytes: bytes) -> bytes:
        """Unpacks circular buffer pointers and reconstructs ordered bytes."""
        if len(slot_bytes) <= 8:
            return b""

        r_ptr, w_ptr = struct.unpack_from("<II", slot_bytes, 0)
        data_area = slot_bytes[8:]
        buf_size = len(data_area)

        # Sanity check pointers
        r_ptr = r_ptr % buf_size if buf_size else 0
        w_ptr = w_ptr % buf_size if buf_size else 0

        if r_ptr == w_ptr:
            # Empty buffer or full readback: if pointers are equal, read entire buffer if non-zero
            return bytes(data_area)

        if r_ptr < w_ptr:
            return bytes(data_area[r_ptr:w_ptr])
        else:
            # Circular wrap-around
            return bytes(data_area[r_ptr:] + data_area[:w_ptr])


# ==============================================================================
# 8. SOF COREDUMP EXTRACTOR
# ==============================================================================

class SofCoredumpLogExtractor:
    """
    Extracts FW logs and SRAM debug slots from sof_dsp_coredump.bin files.
    """
    def extract_from_coredump(self, coredump_bytes: bytes) -> Dict[str, Any]:
        result = {
            "has_debug_window": False,
            "core_logs": {},
            "raw_text": "",
        }
        # Look for Page 0 slot descriptors or ASCII log text within dump
        unpacker = SramDebugWindowUnpacker()
        slots = unpacker.unpack_window(coredump_bytes)
        if slots:
            result["has_debug_window"] = True
            result["core_logs"] = slots

        # Search for ASCII log sequences
        ascii_matches = re.findall(b"[\x20-\x7E\r\n]{10,}", coredump_bytes)
        if ascii_matches:
            result["raw_text"] = "\n".join(
                m.decode("latin1", errors="replace") for m in ascii_matches if len(m) > 20
            )

        return result


# ==============================================================================
# 9. UNIFIED PARSER & CLI CONTROLLER
# ==============================================================================

class SofZephyrLogTool:
    """
    Unified controller for parsing, decoding, streaming, and exporting
    SOF FW Zephyr logs on Windows.
    """
    def __init__(self, elf_path: Optional[str] = None, json_db_path: Optional[str] = None):
        self.database: Optional[ZephyrLogDatabase] = None
        if json_db_path and os.path.exists(json_db_path):
            self.database = ZephyrLogDatabase.from_json_file(json_db_path)
        elif elf_path and os.path.exists(elf_path):
            self.database = ZephyrLogDatabase.from_elf_file(elf_path)

        self.dict_decoder = ZephyrDictionaryDecoder(self.database)
        self.text_decoder = ZephyrTextDecoder()
        self.syst_decoder = MipiSystDecoder()
        self.probe_decoder = SofProbeStreamDecoder()
        self.sram_unpacker = SramDebugWindowUnpacker()
        self.coredump_extractor = SofCoredumpLogExtractor()

    def process_data(
        self,
        raw_bytes: bytes,
        is_sram_window: bool = False,
        is_coredump: bool = False,
        is_probe: bool = False,
        force_text: bool = False,
        force_dict: bool = False,
        min_level: Optional[str] = None,
        filter_module: Optional[str] = None,
        filter_core: Optional[int] = None,
        search_query: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        """Processes raw log bytes through the appropriate decoder pipeline."""
        if is_probe:
            raw_bytes = self.probe_decoder.extract_log_packets(raw_bytes)

        # 1. Check if coredump
        if is_coredump or (len(raw_bytes) > 24 and raw_bytes[:4] == b"SOFD"):
            cd_res = self.coredump_extractor.extract_from_coredump(raw_bytes)
            if cd_res["core_logs"]:
                records = []
                for cid, cbytes in cd_res["core_logs"].items():
                    sub = self.process_data(cbytes, force_text=force_text, force_dict=force_dict)
                    for r in sub:
                        r["core"] = cid
                    records.extend(sub)
                return self._filter_records(records, min_level, filter_module, filter_core, search_query)
            elif cd_res["raw_text"]:
                return self._filter_records(self.text_decoder.decode_text(cd_res["raw_text"]),
                                            min_level, filter_module, filter_core, search_query)

        # 2. Check if SRAM debug window
        if is_sram_window:
            slots = self.sram_unpacker.unpack_window(raw_bytes)
            records = []
            for cid, cbytes in slots.items():
                sub = self.process_data(cbytes, force_text=force_text, force_dict=force_dict)
                for r in sub:
                    r["core"] = cid
                records.extend(sub)
            return self._filter_records(records, min_level, filter_module, filter_core, search_query)

        # 3. Detect format: Binary dictionary vs Plain Text
        is_binary = any(b < 9 or (13 < b < 32) for b in raw_bytes[:min(128, len(raw_bytes))])

        if force_dict or (is_binary and not force_text):
            records = self.dict_decoder.decode_stream(raw_bytes)
            if not records:
                # Try Sys-T fallback
                records = self.syst_decoder.decode_frames(raw_bytes)
        else:
            text = raw_bytes.decode("latin1", errors="replace")
            records = self.text_decoder.decode_text(text)

        return self._filter_records(records, min_level, filter_module, filter_core, search_query)

    def _filter_records(
        self,
        records: List[Dict[str, Any]],
        min_level: Optional[str] = None,
        filter_module: Optional[str] = None,
        filter_core: Optional[int] = None,
        search_query: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        level_prio = {"DBG": 0, "INF": 1, "WRN": 2, "ERR": 3, "FATAL": 4}
        min_prio = level_prio.get((min_level or "DBG").upper(), 0)

        filtered = []
        for r in records:
            # Check level
            lvl = r.get("level") or r.get("severity") or "INF"
            prio = level_prio.get(lvl.upper(), 1)
            if prio < min_prio:
                continue

            # Check core
            if filter_core is not None and r.get("core") is not None and r.get("core") != filter_core:
                continue

            # Check module
            if filter_module:
                mod = r.get("module") or r.get("source") or ""
                if filter_module.lower() not in mod.lower():
                    continue

            # Check search text
            if search_query:
                msg = r.get("message") or r.get("raw") or ""
                if search_query.lower() not in msg.lower():
                    continue

            filtered.append(r)
        return filtered

    def print_records(self, records: List[Dict[str, Any]], color: bool = True):
        """Prints records with clean formatting and colors."""
        reset = "\033[0m" if color else ""
        for r in records:
            c = r.get("color", "") if color else ""
            lvl = r.get("level") or r.get("severity") or "INF"
            ts = r.get("timestamp", "")
            ts_str = f"[{ts}] " if ts else ""
            core = r.get("core")
            core_str = f"(Core {core}) " if core is not None else ""
            mod = r.get("module") or r.get("source") or ""
            mod_str = f"<{mod}>: " if mod else ""
            msg = r.get("message") or r.get("raw") or ""

            print(f"{c}{ts_str}{core_str}{lvl:<3} {mod_str}{msg}{reset}")


# ==============================================================================
# 10. CLI ARGUMENT PARSER & MAIN ENTRYPOINT
# ==============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="SOF Firmware Zephyr Logging & Trace Tooling for Windows",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # 1. Parse raw text trace file:
  python scripts/sof_zephyr_log.py --input C:\\sof_entry.txt

  # 2. Parse binary SRAM debug window dump:
  python scripts/sof_zephyr_log.py --input debug_window.bin --sram

  # 3. Decode dictionary logs using firmware ELF:
  python scripts/sof_zephyr_log.py --input mtrace_core0.bin --elf zephyr.elf

  # 4. Generate JSON dictionary database from ELF:
  python scripts/sof_zephyr_log.py --elf zephyr.elf --gen-db log_database.json

  # 5. Extract and decode logs from crash coredump:
  python scripts/sof_zephyr_log.py --input C:\\sof_dsp_coredump.bin --coredump

  # 6. Run built-in self test verification on Windows:
  python scripts/sof_zephyr_log.py --self-test
        """
    )

    parser.add_argument("-i", "--input", help="Path to input log file (text, bin, coredump, sram)")
    parser.add_argument("-e", "--elf", help="Path to Zephyr firmware ELF binary (zephyr.elf)")
    parser.add_argument("-d", "--db", help="Path to pre-generated Zephyr log_database.json")
    parser.add_argument("--gen-db", help="Generate log_database.json from ELF and save to specified path")

    parser.add_argument("--sram", action="store_true", help="Input is a raw ADSP SRAM Debug Window dump")
    parser.add_argument("--coredump", action="store_true", help="Input is a SOF DSP crash coredump binary")
    parser.add_argument("--probe", action="store_true", help="Input is a SOF Probes DMA stream capture")
    parser.add_argument("--dict", action="store_true", help="Force dictionary binary decoding")
    parser.add_argument("--text", action="store_true", help="Force plain text decoding")

    parser.add_argument("--level", choices=["DBG", "INF", "WRN", "ERR"], help="Filter by minimum log level")
    parser.add_argument("--core", type=int, help="Filter by DSP core ID (e.g. 0, 1, 2, 3)")
    parser.add_argument("--module", help="Filter by log module/source name substring")
    parser.add_argument("--search", help="Search substring in log messages")

    parser.add_argument("--follow", "-f", action="store_true", help="Live tail/stream input file on Windows")
    parser.add_argument("--json", action="store_true", help="Output parsed records as JSON")
    parser.add_argument("--no-color", action="store_true", help="Disable ANSI color output")
    parser.add_argument("--self-test", action="store_true", help="Run comprehensive self-test suite")

    args = parser.parse_args()

    # 1. Handle database generation
    if args.gen_db:
        if not args.elf:
            print("ERROR: --elf <path/to/zephyr.elf> is required to generate database.")
            sys.exit(1)
        print(f"Reading ELF: {args.elf}...")
        reader = ZephyrElfReader(args.elf)
        db = reader.extract_log_database()
        with open(args.gen_db, "w", encoding="utf-8") as f:
            json.dump(db, f, indent=2)
        print(f"Generated {args.gen_db} successfully! ({len(db['strings'])} strings, {len(db['log_subsys']['log_sources'])} sources)")
        sys.exit(0)

    # 2. Handle self-test
    if args.self_test:
        from tests import test_sof_zephyr_log
        sys.exit(test_sof_zephyr_log.run_all_tests())

    # 3. Standard input processing
    if not args.input:
        parser.print_help()
        sys.exit(1)

    if not os.path.exists(args.input):
        print(f"ERROR: Input file not found: {args.input}")
        sys.exit(1)

    tool = SofZephyrLogTool(elf_path=args.elf, json_db_path=args.db)

    # Live streaming mode on Windows
    if args.follow:
        print(f"Tailing {args.input} on Windows (Ctrl+C to exit)...")
        with open(args.input, "rb") as f:
            f.seek(0, os.SEEK_END)
            buf = bytearray()
            try:
                while True:
                    chunk = f.read(4096)
                    if chunk:
                        buf.extend(chunk)
                        # Process newline-delimited chunks
                        if b"\n" in buf or len(buf) > 1024:
                            recs = tool.process_data(
                                bytes(buf),
                                is_sram_window=args.sram,
                                is_coredump=args.coredump,
                                is_probe=args.probe,
                                force_text=args.text,
                                force_dict=args.dict,
                                min_level=args.level,
                                filter_module=args.module,
                                filter_core=args.core,
                                search_query=args.search,
                            )
                            tool.print_records(recs, color=not args.no_color)
                            buf.clear()
                    else:
                        time.sleep(0.1)
            except KeyboardInterrupt:
                print("\nStream stopped.")
                sys.exit(0)

    # Static file mode
    with open(args.input, "rb") as f:
        data = f.read()

    records = tool.process_data(
        data,
        is_sram_window=args.sram,
        is_coredump=args.coredump,
        is_probe=args.probe,
        force_text=args.text,
        force_dict=args.dict,
        min_level=args.level,
        filter_module=args.module,
        filter_core=args.core,
        search_query=args.search,
    )

    if args.json:
        # Filter raw_data byte arrays from JSON export
        clean_recs = []
        for r in records:
            cr = dict(r)
            if "raw_data" in cr:
                del cr["raw_data"]
            clean_recs.append(cr)
        print(json.dumps(clean_recs, indent=2))
    else:
        tool.print_records(records, color=not args.no_color)


if __name__ == "__main__":
    main()
