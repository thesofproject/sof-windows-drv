#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 The SOF Authors
#
# Automated Test Suite for SOF Firmware Zephyr Logging Tooling on Windows.
# Validates all Zephyr logging modes (Text, Dictionary v3, MIPI Sys-T, Probes,
# SRAM Debug Window, ELF Database generation, and Coredump extraction) locally on Windows.
#
# DUT is off - all tests execute entirely in local memory / host environment.

import json
import os
import struct
import sys
import unittest

# Add scripts directory to module path
current_dir = os.path.dirname(os.path.abspath(__file__))
scripts_dir = os.path.join(os.path.dirname(current_dir), "scripts")
if scripts_dir not in sys.path:
    sys.path.insert(0, scripts_dir)

from sof_zephyr_log import (
    MipiSystDecoder,
    SofCoredumpLogExtractor,
    SofProbeStreamDecoder,
    SofZephyrLogTool,
    SramDebugWindowUnpacker,
    ZephyrDictionaryDecoder,
    ZephyrElfReader,
    ZephyrLogDatabase,
    ZephyrTextDecoder,
)


class TestZephyrLoggingTool(unittest.TestCase):
    """Unit and integration tests for Windows SOF Zephyr Log Tooling."""

    REAL_ELF_PATH = r"\\192.168.122.1\work\sof-ptl\build-ptl\zephyr\zephyr.elf"

    def test_01_text_decoder(self):
        """Validates decoding of standard Zephyr text formats with timestamps and levels."""
        raw_text = (
            "<inf> [   12.345678] <basefw>: DSP core 0 scheduler running\n"
            "<err> [00:01:23.456,789] (Core 1) <pipeline>: Buffer underrun on pipe 2\n"
            "<wrn> <dai>: ALH link clock skew detected\n"
            "<dbg> [    0.001200] (Core 0) <ipc>: IPC message processed\n"
            "Unformatted boot banner line without tags\n"
        )
        decoder = ZephyrTextDecoder()
        records = decoder.decode_text(raw_text)

        self.assertEqual(len(records), 5)
        # Record 0
        self.assertEqual(records[0]["level"], "INF")
        self.assertEqual(records[0]["timestamp"], "12.345678")
        self.assertEqual(records[0]["module"], "basefw")
        self.assertEqual(records[0]["message"], "DSP core 0 scheduler running")

        # Record 1
        self.assertEqual(records[1]["level"], "ERR")
        self.assertEqual(records[1]["timestamp"], "00:01:23.456,789")
        self.assertEqual(records[1]["core"], 1)
        self.assertEqual(records[1]["module"], "pipeline")
        self.assertIn("Buffer underrun", records[1]["message"])

        # Record 2
        self.assertEqual(records[2]["level"], "WRN")
        self.assertEqual(records[2]["module"], "dai")

        # Record 3
        self.assertEqual(records[3]["level"], "DBG")
        self.assertEqual(records[3]["core"], 0)

        # Record 4 (fallback unformatted)
        self.assertEqual(records[4]["module"], "raw")
        self.assertIn("Unformatted", records[4]["message"])
        print("[PASS] test_01_text_decoder: Verified multi-format Zephyr text parsing.")

    def test_02_pure_python_elf_reader_real(self):
        """Validates pure-Python ELF reader on real SOF Panther Lake zephyr.elf."""
        if not os.path.exists(self.REAL_ELF_PATH):
            self.skipTest(f"Real ELF not found at {self.REAL_ELF_PATH}")

        reader = ZephyrElfReader(self.REAL_ELF_PATH)
        self.assertFalse(reader.is_64bit)
        self.assertTrue(reader.is_little_endian)

        # Verify crucial SOF sections
        self.assertIn(".rodata", reader.sections)
        self.assertIn("log_const_area", reader.sections)
        self.assertIn(".symtab", reader.sections)
        self.assertIn(".strtab", reader.sections)

        # Verify string lookup for SOF modules
        rodata_sec = reader.sections[".rodata"]
        self.assertGreater(rodata_sec["size"], 1000)

        # Generate database dictionary
        db_dict = reader.extract_log_database()
        self.assertEqual(db_dict["version"], 3)
        self.assertEqual(db_dict["target"]["bits"], 32)
        self.assertTrue(db_dict["target"]["little_endian"])
        self.assertGreater(len(db_dict["strings"]), 50)
        self.assertGreater(len(db_dict["log_subsys"]["log_sources"]), 5)

        # Verify known module names extracted
        sources = [v["name"] for v in db_dict["log_subsys"]["log_sources"].values()]
        self.assertTrue(any("basefw" in s or "ace" in s for s in sources))
        print(f"[PASS] test_02_pure_python_elf_reader_real: Extracted {len(db_dict['strings'])} strings and {len(sources)} sources from real zephyr.elf.")

    def test_03_dictionary_decoder_v3(self):
        """Validates Zephyr Dictionary v3 binary packet decoding with argument interpolation."""
        # Create an in-memory database
        fmt_addr = 0xA0091000
        src_addr = 0xA00B2000
        str_pcm = 0xA0092000

        mock_db = ZephyrLogDatabase({
            "version": 3,
            "target": {"bits": 32, "little_endian": True},
            "build_id": "test_build_123",
            "strings": {
                f"0x{fmt_addr:08x}": "Stream #%d active: rate=%u Hz, channels=%d, format=%s",
                f"0x{str_pcm:08x}": "PCM_S24_4LE",
            },
            "log_subsys": {
                "log_sources": {
                    f"0x{src_addr:08x}": {"name": "audio_engine", "index": 0}
                }
            }
        })

        decoder = ZephyrDictionaryDecoder(mock_db)

        # Construct cbvprintf package:
        #   [fmt_ptr: 4B]
        #   [arg1: int32 = 1]
        #   [arg2: uint32 = 48000]
        #   [arg3: int32 = 2]
        #   [arg4: ptr = str_pcm]
        pkg_data = struct.pack("<IIIII", fmt_addr, 1, 48000, 2, str_pcm)

        # Construct v3 message header:
        #   uint8_t msg_type = 0;
        #   uint8_t domain_lvl = (3 << 4) | 0; (level 3 = INF, domain 0)
        #   uint16_t pkg_len = len(pkg_data);
        #   uint16_t data_len = 0;
        #   uint32_t source = src_addr;
        #   uint64_t timestamp = 987654321;
        raw_pkt = bytearray()
        raw_pkt.append(0) # msg_type normal
        raw_pkt.extend(struct.pack("<BHH", (3 << 4) | 0, len(pkg_data), 0))
        raw_pkt.extend(struct.pack("<IQ", src_addr, 987654321))
        raw_pkt.extend(pkg_data)

        # Also append a dropped packet
        raw_pkt.append(1) # dropped
        raw_pkt.extend(struct.pack("<H", 4))

        records = decoder.decode_stream(bytes(raw_pkt))
        self.assertEqual(len(records), 2)

        r0 = records[0]
        self.assertEqual(r0["type"], "normal")
        self.assertEqual(r0["level"], "INF")
        self.assertEqual(r0["source"], "audio_engine")
        self.assertEqual(r0["timestamp"], 987654321)
        self.assertEqual(r0["message"], "Stream #1 active: rate=48000 Hz, channels=2, format=PCM_S24_4LE")

        r1 = records[1]
        self.assertEqual(r1["type"], "dropped")
        self.assertEqual(r1["count"], 4)
        print("[PASS] test_03_dictionary_decoder_v3: Successfully decoded binary dictionary packet and argument expansion.")

    def test_04_sram_debug_window_unpacker(self):
        """Validates multi-core slot discovery and circular wrap-around unpacking in SRAM debug window."""
        unpacker = SramDebugWindowUnpacker()

        # Construct Page 0 with descriptors for Core 0 and Core 1
        page0 = bytearray(4096)
        # Slot 0 (page 1) -> Core 0 (0x474f4c00)
        struct.pack_into("<III", page0, 0 * 12, 0, 0x474f4c00, 0)
        # Slot 1 (page 2) -> Core 1 (0x474f4c01)
        struct.pack_into("<III", page0, 1 * 12, 1, 0x474f4c01, 0)

        # Slot 0: Linear data
        msg_core0 = b"<inf> Core 0 running normally\n"
        slot0 = bytearray(4096)
        struct.pack_into("<II", slot0, 0, 0, len(msg_core0))
        slot0[8 : 8 + len(msg_core0)] = msg_core0

        # Slot 1: Circular wrap-around data
        # Buf size = 4088. Suppose r_ptr = 4070, w_ptr = len(part2)
        slot1 = bytearray(4096)
        part1 = b"[WRAP1]"
        part2 = b"[WRAP2]"
        r_ptr = 4070
        w_ptr = len(part2)
        struct.pack_into("<II", slot1, 0, r_ptr, w_ptr)
        slot1[8 + r_ptr : 8 + r_ptr + len(part1)] = part1
        slot1[8 : 8 + len(part2)] = part2

        full_window = bytes(page0 + slot0 + slot1)
        extracted = unpacker.unpack_window(full_window)

        self.assertIn(0, extracted)
        self.assertIn(1, extracted)
        self.assertEqual(extracted[0], msg_core0)
        self.assertTrue(extracted[1].startswith(part1))
        self.assertTrue(extracted[1].endswith(part2))
        print("[PASS] test_04_sram_debug_window_unpacker: Successfully unpacked multi-core slots with circular wrap-around.")

    def test_05_mipi_syst_decoder(self):
        """Validates MIPI Sys-T protocol catalog and short frame decoding."""
        decoder = MipiSystDecoder()

        # Frame 1: Catalog message (type=1, severity=3 WRN, len=4, catalog_id=0x12345678)
        hdr1 = (1 & 0xF) | ((3 & 0xF) << 4) | ((4 & 0xFF) << 8)
        frame1 = struct.pack("<II", hdr1, 0x12345678)

        # Frame 2: Short message (type=2, severity=2 ERR, len=4, val=0x0000002A)
        hdr2 = (2 & 0xF) | ((2 & 0xF) << 4) | ((4 & 0xFF) << 8)
        frame2 = struct.pack("<II", hdr2, 42)

        recs = decoder.decode_frames(frame1 + frame2)
        self.assertEqual(len(recs), 2)
        self.assertEqual(recs[0]["severity"], "WRN")
        self.assertEqual(recs[0]["catalog_id"], "0x12345678")
        self.assertEqual(recs[1]["severity"], "ERR")
        self.assertEqual(recs[1]["value"], 42)
        print("[PASS] test_05_mipi_syst_decoder: Decoded MIPI Sys-T catalog and short frames.")

    def test_06_sof_probe_decoder(self):
        """Validates extraction of inner log payload from SOF Probe DMA packets."""
        probe_decoder = SofProbeStreamDecoder()

        log_payload = b"<inf> [0.001] Probe log message\n"
        # Probe packet header:
        #   uint32_t sync_and_size = len(log_payload)
        #   uint16_t stream_tag = 3
        #   uint16_t purpose = PROBE_PURPOSE_LOGGING (1)
        probe_pkt = bytearray()
        probe_pkt.extend(struct.pack("<IHH", len(log_payload), 3, 1))
        probe_pkt.extend(log_payload)

        extracted = probe_decoder.extract_log_packets(bytes(probe_pkt))
        self.assertEqual(extracted, log_payload)
        print("[PASS] test_06_sof_probe_decoder: Extracted log stream from SOF Probe DMA framing.")

    def test_07_coredump_log_extractor(self):
        """Validates extraction of logs from SOF DSP crash coredump binary."""
        extractor = SofCoredumpLogExtractor()

        # Construct synthetic coredump binary
        # Header (64 bytes) with "SOFD" magic
        cd_data = bytearray(b"SOFD" + b"\x00" * 60)
        # Mailbox Uplink & Downlink (4096 * 2)
        cd_data.extend(b"\x00" * 8192)
        # Appended text trace
        cd_data.extend(b"[DSP-PANIC] Exception caught on DSP core 0: EXCCAUSE=28, EPC1=0xa0054320\n")

        res = extractor.extract_from_coredump(bytes(cd_data))
        self.assertIn("EXCCAUSE=28", res["raw_text"])
        print("[PASS] test_07_coredump_log_extractor: Extracted crash diagnostics from coredump.")

    def test_08_unified_tool_controller(self):
        """Validates end-to-end processing and filtering through SofZephyrLogTool."""
        tool = SofZephyrLogTool()

        raw_stream = (
            "<err> (Core 0) <basefw>: Critical DSP failure\n"
            "<inf> (Core 0) <pipeline>: Pipeline 1 started\n"
            "<dbg> (Core 1) <stream>: Processing 192 frames\n"
        )
        # Test filtering by minimum level ERR
        recs = tool.process_data(raw_stream.encode("utf-8"), min_level="ERR")
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0]["level"], "ERR")

        # Test filtering by Core 1
        recs_core1 = tool.process_data(raw_stream.encode("utf-8"), filter_core=1)
        self.assertEqual(len(recs_core1), 1)
        self.assertEqual(recs_core1[0]["module"], "stream")

        # Test search query
        recs_search = tool.process_data(raw_stream.encode("utf-8"), search_query="Pipeline")
        self.assertEqual(len(recs_search), 1)
        self.assertIn("Pipeline 1 started", recs_search[0]["message"])
        print("[PASS] test_08_unified_tool_controller: Verified multi-stage filtering and query engine.")


def run_all_tests() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TestZephyrLoggingTool)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run_all_tests())
