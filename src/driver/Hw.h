/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Hw.h
 * @brief Hardware Architecture, Register Definitions, and Platform Descriptors
 *        for Intel cAVS, Intel ACE, and AMD Audio DSPs.
 *
 * This header declares hardware descriptors, MMIO register offsets, bitmask
 * definitions, and low-level IP access routines for controlling Intel and AMD
 * audio DSP subsystems on Windows.
 *
 * Architecture Overview:
 * - BAR0 (0x0000 - 0x1FFF): Intel High Definition Audio (HDA) Controller Registers.
 *   Hosts the Global Control (GCTL), Stream Descriptors (SD_CTL, SD_BDL, SD_LPIB),
 *   Processing Pipe Capabilities (PP), and Vendor-Specific Power (D0i3C).
 * - BAR4 (0x00000 - 0x1FFFFF): DSP Subsystem MMIO, SRAM Windows & Mailboxes.
 *   Hosts the Shim registers, Core Power rails, Mailbox Downlink (Host -> DSP),
 *   Mailbox Uplink (DSP -> Host), and Doorbell registers (HIPC).
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#ifndef __SOF_HW_H__
#define __SOF_HW_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#include <windows.h>
#endif
#include <sof/sof_osal.h>
#include "PciIds.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum _SOF_HW_FAMILY
 * @brief Categorizes supported hardware microarchitecture generations.
 *
 * Each family exhibits distinct memory window layouts, core power sequencing,
 * interrupt delivery mechanisms, and mailbox doorbell protocol variations.
 */
typedef enum _SOF_HW_FAMILY {
    /** @brief Unrecognized or uninitialized hardware platform. */
    SOF_HW_UNKNOWN = 0,

    /** @brief Intel cAVS 1.5 microarchitecture (Apollo Lake APL, Gemini Lake GLK).
     *  Features 2 DSP cores, 40h doorbell base, 80000h SRAM status window. */
    SOF_HW_INTEL_CAVS_1_5,

    /** @brief Intel cAVS 1.8 microarchitecture (Cannon Lake CNL, Comet Lake CML).
     *  Introduces 4 DSP cores, 0xC0 doorbell base, and extended IPC registers. */
    SOF_HW_INTEL_CAVS_1_8,

    /** @brief Intel cAVS 2.0 microarchitecture (Ice Lake ICL, Jasper Lake JSL, Elkhart Lake EHL).
     *  Adds SoundWire master integration and refined power-gating states. */
    SOF_HW_INTEL_CAVS_2_0,

    /** @brief Intel cAVS 2.5 microarchitecture (Tiger Lake TGL, Alder Lake ADL, Raptor Lake RPL).
     *  Features IPC4 firmware architecture, high-bandwidth Link DMA, and SoundWire Multi-Link. */
    SOF_HW_INTEL_CAVS_2_5,

    /** @brief Intel ACE 1.0 / 1.5 microarchitecture (Meteor Lake MTL, Arrow Lake ARL).
     *  Advanced DSP Core Engine with HFDSSCS power gating, 73200h HFIPCX doorbells,
     *  and 188000h Mailbox Downlink window. */
    SOF_HW_INTEL_ACE_1_0,

    /** @brief Intel ACE 2.0 microarchitecture (Lunar Lake LNL).
     *  5 DSP cores, 160200h Extended ROM / Core status registers, and low-power SRAM tiles. */
    SOF_HW_INTEL_ACE_2_0,

    /** @brief Intel ACE 3.0 microarchitecture (Panther Lake PTL, Wildcat Lake WCL).
     *  Next-gen audio subsystem with dual HFPWRCTL2 power rails and hardware UAOL offload. */
    SOF_HW_INTEL_ACE_3_0,

    /** @brief Intel ACE 4.0 microarchitecture (Nova Lake NVL, Nova Lake-S NVL-S).
     *  Advanced Core Engine 4.0 featuring 4 DSP cores (Mobile) / 2 cores (Desktop),
     *  0x160200 extended ROM status registers, and PTL-derived hardware ops. */
    SOF_HW_INTEL_ACE_4_0,

    /** @brief AMD Audio Co-Processor (ACP) subsystem for Ryzen APUs. */
    SOF_HW_AMD_ACP,
} SOF_HW_FAMILY;

/**
 * @struct _SOF_DSP_HW_DESC
 * @brief Hardware capabilities, register map offsets, and mailbox layout for a platform.
 *
 * Dynamically resolved at driver initialization via PCI Vendor/Device ID matching.
 */
typedef struct _SOF_DSP_HW_DESC {
    /** @brief Hardware microarchitecture family identifier. */
    SOF_HW_FAMILY   Family;

    /** @brief Human-readable platform name string for diagnostic logging. */
    PCSTR           PlatformName;

    /** @brief Number of DSP cores present in the audio processing subsystem. */
    ULONG           CoresNum;

    /* --- Mailbox & SRAM offsets relative to BAR4 --- */

    /** @brief BAR4 byte offset to the ROM Status Register (monitors boot progress). */
    ULONG           RomStatusReg;

    /** @brief BAR4 byte offset to the ROM Error Register (contains panic/error codes). */
    ULONG           RomErrorReg;

    /** @brief BAR4 byte offset to the FW Status Register (live firmware state). */
    ULONG           FwStatusReg;

    /** @brief BAR4 byte offset to the Downlink Mailbox (Host -> DSP command payload). */
    ULONG           MailboxDownlinkOffset;

    /** @brief Size in bytes of the Downlink Mailbox buffer (typically 4096 bytes). */
    ULONG           MailboxDownlinkSize;

    /** @brief BAR4 byte offset to the Uplink Mailbox (DSP -> Host notification payload). */
    ULONG           MailboxUplinkOffset;

    /** @brief Size in bytes of the Uplink Mailbox buffer (typically 4096 bytes). */
    ULONG           MailboxUplinkSize;

    /* --- Doorbell registers relative to BAR4 --- */

    /** @brief Transmit Data register (DSP -> Host notification primary word). */
    ULONG           HipcTdr;

    /** @brief Transmit Acknowledge register (DSP -> Host clear/done acknowledgment). */
    ULONG           HipcTda;

    /** @brief Transmit Extension Data register (DSP -> Host extension payload word). */
    ULONG           HipcTdd;

    /** @brief Host Interrupt Data register (Host -> DSP command primary word). */
    ULONG           HipcIdr;

    /** @brief Host Interrupt Acknowledge register (Host -> DSP completion acknowledgment). */
    ULONG           HipcIda;

    /** @brief Host Interrupt Extension Data register (Host -> DSP extension word). */
    ULONG           HipcIdd;

    /** @brief IPC Control register (interrupt unmask and edge triggers). */
    ULONG           HipcCtl;

    /* --- Doorbell bitmasks --- */

    /** @brief Bitmask indicating DSP has pending data in HipcTdr (Busy bit). */
    ULONG           HipcTdrBusy;

    /** @brief Bitmask indicating Host has acknowledged DSP notification in HipcTda (Done bit). */
    ULONG           HipcTdaDone;

    /** @brief Bitmask indicating Host has sent command in HipcIdr (Busy bit asserted). */
    ULONG           HipcIdrBusy;

    /** @brief Bitmask indicating DSP has completed Host command in HipcIda (Done bit). */
    ULONG           HipcIdaDone;

    /** @brief Control register mask for enabling Busy interrupts. */
    ULONG           HipcCtlBusy;

    /** @brief Control register mask for enabling Done interrupts. */
    ULONG           HipcCtlDone;

    /* --- ACE-specific Interrupt Hierarchy (Meteor Lake / Lunar Lake / Panther Lake) --- */

    /** @brief TRUE if platform uses Intel ACE interrupt steering hierarchy. */
    BOOLEAN         IsAce;

    /** @brief Offset to ACE Interrupt IP Pointer register (MTL_HFINTIPPTR, typically 0x1108). */
    ULONG           IntIpPtrReg;

    /** @brief Offset within steered block to Interrupt Status register (typically 0x20). */
    ULONG           IrqStsOffset;

    /** @brief Mask within IrqSts register indicating IPC interrupt pending (BIT 0). */
    ULONG           IrqStsIpcMask;

} SOF_DSP_HW_DESC, *PSOF_DSP_HW_DESC;

/* =========================================================================
 * Intel HDA Controller Registers (BAR0)
 * ========================================================================= */

/** @brief Global Capabilities register (BAR0: 0x00). Reports stream channels & features. */
#define SOF_HDA_GCAP                        0x00

/** @brief Global Control register (BAR0: 0x08). Controls controller reset state. */
#define SOF_HDA_GCTL                        0x08

/** @brief Global Control Reset bit. Write 0 to enter reset; write 1 and poll to exit. */
#define SOF_HDA_GCTL_RESET                  0x00000001

/** @brief Wake Status register (BAR0: 0x0E). Indicates codec or link wake events. */
#define SOF_HDA_WAKESTS                     0x0E

/** @brief Wake event detected on Serial Data In 0 (SDIN0). */
#define SOF_HDA_WAKESTS_SDIN0               0x0001
/** @brief Wake event detected on Serial Data In 1 (SDIN1). */
#define SOF_HDA_WAKESTS_SDIN1               0x0002
/** @brief Wake event detected on Serial Data In 2 (SDIN2). */
#define SOF_HDA_WAKESTS_SDIN2               0x0004
/** @brief Wake event detected on Serial Data In 3 (SDIN3). */
#define SOF_HDA_WAKESTS_SDIN3               0x0008

/** @brief Interrupt Control register (BAR0: 0x20). Master interrupt enable for controller. */
#define SOF_HDA_INTCTL                      0x20

/** @brief Global Interrupt Enable mask (INTCTL BIT 31). */
#define SOF_HDA_INT_GLOBAL_EN               0x80000000

/** @brief Controller Interrupt Enable mask (INTCTL BIT 30). */
#define SOF_HDA_INT_CTRL_EN                 0x40000000

/* =========================================================================
 * PCI Extended Capabilities Discovery (BAR0)
 * ========================================================================= */

/** @brief Maximum number of extended capabilities to traverse during BAR0 capability walking. */
#define SOF_HDA_MAX_CAPS                    10

/** @brief Bit offset of Capability ID in 32-bit capability header. */
#define SOF_HDA_CAP_ID_OFF                  16

/** @brief Mask for extracting 12-bit Capability ID from capability header DWORD. */
#define SOF_HDA_CAP_ID_MASK                 0x0FFF0000

/** @brief Mask for extracting next capability offset pointer from lower 16 bits. */
#define SOF_HDA_CAP_NEXT_MASK               0xFFFF

/** @brief Multi-Link Capability ID (0x2). Defines additional digital audio links. */
#define SOF_HDA_ML_CAP_ID                   0x2

/** @brief Processing Pipe Capability ID (0x3). Connects host DMA streams to DSP pipeline. */
#define SOF_HDA_PP_CAP_ID                   0x3

/** @brief Software Position in Buffer (SPIB) Capability ID (0x4). High-resolution DMA ptrs. */
#define SOF_HDA_SPIB_CAP_ID                 0x4

/** @brief Dynamic Render Stream Mapping (DRSM) Capability ID (0x5). */
#define SOF_HDA_DRSM_CAP_ID                 0x5

/* --- Processing Pipe (PP) Capability Registers --- */

/** @brief Offset to Processing Pipe Control register relative to PP capability base. */
#define SOF_HDA_REG_PP_PPCTL                0x04

/** @brief Processing Pipe Global Enable mask. Enables DSP processing pipe routing. */
#define SOF_HDA_PPCTL_GPROCEN               0x40000000

/** @brief Processing Pipe Interrupt Enable mask. */
#define SOF_HDA_PPCTL_PIE                   0x80000000

/** @brief Base offset of Host Processing Pipe Channel descriptors relative to PP base. */
#define SOF_HDA_PPHC_BASE                   0x10

/** @brief Stride interval in bytes between successive Host Processing Pipe Channels. */
#define SOF_HDA_PPHC_INTERVAL               0x10

/** @brief Base offset of Link Processing Pipe Channel descriptors relative to PP base. */
#define SOF_HDA_PPLC_BASE                   0x10

/** @brief Multiplier stride for Link Processing Pipe Channels. */
#define SOF_HDA_PPLC_MULTI                  0x10

/** @brief Interval in bytes between Link Processing Pipe Channels. */
#define SOF_HDA_PPLC_INTERVAL               0x10

/** @brief Link Processing Pipe Control register offset. */
#define SOF_HDA_REG_PPLCCTL                 0x00

/** @brief Link Channel Stream Reset bit. */
#define SOF_HDA_PPLCCTL_STRST               0x00000001

/** @brief Link Channel Stream Run bit. Set to start DMA transfers on link side. */
#define SOF_HDA_PPLCCTL_RUN                 0x00000002

/** @brief Stream Tag mask within Link Channel Control register. */
#define SOF_HDA_PPLCCTL_STRM_MASK           0x00F00000

/** @brief Shift count for Stream Tag field in Link Channel Control register. */
#define SOF_HDA_PPLCCTL_STRM_SHIFT          20

/** @brief Link Channel Audio Format register offset. */
#define SOF_HDA_REG_PPLCFMT                 0x04

/** @brief Link Channel Lower Base Address register offset (BDL pointer low). */
#define SOF_HDA_REG_PPLCLLPL                0x08

/** @brief Link Channel Upper Base Address register offset (BDL pointer high). */
#define SOF_HDA_REG_PPLCLLPU                0x0C

/* --- Multi-Link (ML) Capability Registers --- */

/** @brief Base offset of Multi-Link descriptors relative to ML capability header. */
#define SOF_HDA_ML_BASE                     0x40

/** @brief Stride interval in bytes between Multi-Link link instances. */
#define SOF_HDA_ML_INTERVAL                 0x40

/** @brief Multi-Link Capabilities register offset. */
#define SOF_HDA_REG_ML_LCAP                 0x00

/** @brief Multi-Link Control register offset. Controls link power and clock. */
#define SOF_HDA_REG_ML_LCTL                 0x04

/** @brief Multi-Link Output Stream ID Valid register offset. */
#define SOF_HDA_REG_ML_LOSIDV               0x08

/** @brief Multi-Link Input Stream ID register offset. */
#define SOF_HDA_REG_ML_LSDIID               0x0C

/** @brief Multi-Link Stream Power Active bit (BIT 16). Asserts link power. */
#define SOF_HDA_MLCTL_SPA                   (1 << 16)

/** @brief Multi-Link Current Power Active bit (BIT 23). Readback status for power gating. */
#define SOF_HDA_MLCTL_CPA                   (1 << 23)

/* --- Software Position In Buffer (SPIB) Registers --- */

/** @brief Software Position Buffer FIFO Control register offset. */
#define SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL     0x04

/** @brief Base offset for SPIB position registers. */
#define SOF_HDA_SPIB_BASE                   0x08

/** @brief Interval in bytes between SPIB position entries. */
#define SOF_HDA_SPIB_INTERVAL               0x08

/** @brief SPIB register offset (reports host position in DMA buffer). */
#define SOF_HDA_SPIB_SPIB                   0x00

/** @brief Max FIFO size register offset. */
#define SOF_HDA_SPIB_MAXFIFO                0x04

/* =========================================================================
 * Intel HDA DMA Stream Descriptor Registers (BAR0)
 * ========================================================================= */

/** @brief Base offset of HDA Host DMA Stream Descriptors in BAR0 (SD0 at 0x80). */
#define SOF_HDA_ADSP_LOADER_BASE            0x80

/** @brief Size in bytes of each HDA Stream Descriptor register block (0x20 bytes). */
#define SOF_HDA_ADSP_SD_ENTRY_SIZE          0x20

/** @brief Stream Descriptor Control register (SD_CTL: Run, Reset, Interrupt Enables). */
#define SOF_HDA_ADSP_REG_SD_CTL             0x00

/** @brief Stream Descriptor Status register (SD_STS: Buffer Completion, FIFO Error). */
#define SOF_HDA_ADSP_REG_SD_STS             0x03

/** @brief Link Position In Buffer (SD_LPIB: Hardware DMA read/write pointer). */
#define SOF_HDA_ADSP_REG_SD_LPIB            0x04

/** @brief Cyclic Buffer Length in bytes (SD_CBL: Total ring buffer size). */
#define SOF_HDA_ADSP_REG_SD_CBL             0x08

/** @brief Last Valid Index in BDL table (SD_LVI: Number of BDL entries minus 1). */
#define SOF_HDA_ADSP_REG_SD_LVI             0x0C

/** @brief FIFO Size in bytes (SD_FIFOSIZE: Hardware internal FIFO capacity). */
#define SOF_HDA_ADSP_REG_SD_FIFOSIZE        0x10

/** @brief Stream Format register (SD_FORMAT: Channels, bits/sample, sample rate). */
#define SOF_HDA_ADSP_REG_SD_FORMAT          0x12

/** @brief Buffer Descriptor List Pointer Lower 32-bits (SD_BDLPL: 128-byte aligned). */
#define SOF_HDA_ADSP_REG_SD_BDLPL           0x18

/** @brief Buffer Descriptor List Pointer Upper 32-bits (SD_BDLPU: Physical address high). */
#define SOF_HDA_ADSP_REG_SD_BDLPU           0x1C

/** @brief Default Code Loader stream format (48kHz, 16-bit, stereo, Tag 1). */
#define SOF_HDA_CL_STREAM_FORMAT            0x0040

/** @brief Stream Control DMA Reset bit (SD_CTL BIT 0). */
#define SOF_HDA_SD_CTL_DMA_RESET            0x01

/** @brief Stream Control DMA Run bit (SD_CTL BIT 1). */
#define SOF_HDA_SD_CTL_DMA_START            0x02

/** @brief Stream Control interrupt mask (IOCE + FEIE + DEIE). */
#define SOF_HDA_CL_DMA_SD_INT_MASK          0x1C

/** @brief Lower Link Capabilities Header offset (BAR0: 0x14). */
#define SOF_HDA_LLCH                        0x14

/* =========================================================================
 * Buffer Descriptor List (BDL) Structure
 * ========================================================================= */

#include <pshpack1.h>
/**
 * @struct _SOF_INTEL_DSP_BDL
 * @brief Represents a single 16-byte Buffer Descriptor List (BDL) entry in host memory.
 *
 * HDA DMA controllers traverse cyclic BDL arrays to transfer audio packets between
 * system RAM and hardware FIFOs without CPU intervention.
 */
typedef struct _SOF_INTEL_DSP_BDL {
    /** @brief Physical 32-bit lower address of the contiguous memory buffer chunk. */
    ULONG AddrLow;

    /** @brief Physical 32-bit upper address (for 64-bit physical address spaces). */
    ULONG AddrHigh;

    /** @brief Length in bytes of this buffer segment (must be sample-aligned). */
    ULONG Size;

    /** @brief Interrupt on Completion (IOC) flag. Set to 1 to fire interrupt on boundary. */
    ULONG Ioc;
} SOF_INTEL_DSP_BDL, *PSOF_INTEL_DSP_BDL;
#include <poppack.h>

/* =========================================================================
 * ROM Control Commands & cAVS DSP Power Sequencing (BAR4)
 * ========================================================================= */

/** @brief ROM IPC command identifier used during firmware download handshake. */
#define HDA_DSP_ROM_IPC_CONTROL             0x01000000

/** @brief Purge firmware command bit passed to ROM loader. */
#define HDA_DSP_ROM_IPC_PURGE_FW            0x00004000

/** @brief cAVS Audio DSP Control and Status register (BAR4: 0x04). */
#define HDA_DSP_REG_ADSPCS                  0x04

/** @brief Core Reset bitmask macro for cAVS. Asserts core reset when set to 0. */
#define HDA_DSP_ADSPCS_CRST_MASK(cm)        ((cm) << 0)

/** @brief Core Stall bitmask macro for cAVS. Halts execution when set to 1. */
#define HDA_DSP_ADSPCS_CSTALL_MASK(cm)      ((cm) << 8)

/** @brief Set Power Active bitmask macro for cAVS. Requests power rail activation. */
#define HDA_DSP_ADSPCS_SPA_MASK(cm)         ((cm) << 16)

/** @brief Current Power Active bitmask macro for cAVS. Polled for power rail readiness. */
#define HDA_DSP_ADSPCS_CPA_MASK(cm)         ((cm) << 24)

/* =========================================================================
 * Intel ACE DSP Subsystem Power & Domain Gating (BAR4)
 * ========================================================================= */

/** @brief ACE High Performance Subsystem Control & Status register (BAR4: 0x1000). */
#define MTL_HFDSSCS                         0x1000

/** @brief Subsystem Set Power Active bit (HFDSSCS BIT 16). */
#define MTL_HFDSSCS_SPA_MASK                (1 << 16)

/** @brief Subsystem Current Power Active bit (HFDSSCS BIT 24). Polled after setting SPA. */
#define MTL_HFDSSCS_CPA_MASK                (1 << 24)

/** @brief ACE Core Power Control register (BAR4: 0x1D18). Controls HP core power rails. */
#define MTL_HFPWRCTL                        0x1D18

/** @brief ACE Core Power Status register (BAR4: 0x1D1C). Polled for rail stabilization. */
#define MTL_HFPWRSTS                        0x1D1C

/** @brief PTL Secondary Core Power Control register (BAR4: 0x1D20). */
#define PTL_HFPWRCTL2                       0x1D20

/** @brief PTL Secondary Core Power Status register (BAR4: 0x1D24). */
#define PTL_HFPWRSTS2                       0x1D24

/** @brief Wake/Power DSP High Performance Core Power Gate enable (BIT 0). */
#define MTL_HFPWRCTL_WPDSPHPXPG             (1 << 0)

/** @brief DSP High Performance Core Power Gate Status mask (BIT 0). */
#define MTL_HFPWRSTS_DSPHPXPGS_MASK         (1 << 0)

/* =========================================================================
 * Vendor-Specific D0i3 Power Handshake (BAR0)
 * ========================================================================= */

/** @brief Vendor-Specific D0i3 Control register offset in BAR0 (MTL / LNL / PTL). */
#define MTL_HDA_VS_D0I3C                    0x1D4A

/** @brief Command In Progress (CIP) bit in D0i3C. Polled to verify handshake completion. */
#define SOF_HDA_VS_D0I3C_CIP                0x0001

/** @brief D0i3 State Request bit (I3). Write 1 to enter low-power D0i3 state; 0 for D0. */
#define SOF_HDA_VS_D0I3C_I3                 0x0004

/* =========================================================================
 * Intel ACE Primary Core Execution & Status (BAR4)
 * ========================================================================= */

/** @brief Primary DSP Core Control register in ACE (BAR4: 0x178D04). */
#define MTL_DSP2CXCTL_PRIMARY_CORE          0x178D04

/** @brief Primary Core Set Power Active bit (BIT 0). */
#define MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK 0x00000001

/** @brief Primary Core Current Power Active bit (BIT 8). */
#define MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK 0x00000100

/** @brief Operating Select bits in DSP2CXCTL (controls core unreset mode). */
#define MTL_DSP2CXCTL_PRIMARY_CORE_OSEL     0x03000000

/** @brief Shift count for Operating Select bits in DSP2CXCTL. */
#define MTL_DSP2CXCTL_PRIMARY_CORE_OSEL_SHIFT 24

/** @brief LNL / PTL Hardware Firmware Extended Status register (BAR4: 0x160200). */
#define LNL_DSP_REG_HFDSC                   0x160200

/** @brief LNL / PTL Hardware Firmware Extended Error register (BAR4: 0x160204). */
#define LNL_DSP_REG_HFDEC                   0x160204

/* =========================================================================
 * Intel ACE Host IPC Interrupt Steering (BAR4)
 * ========================================================================= */

/** @brief Host Interrupt IP Pointer register (BAR4: 0x1108). */
#define MTL_HFINTIPPTR                      0x1108

/** @brief Mask to extract 21-bit pointer offset to active interrupt status block. */
#define MTL_HFINTIPPTR_PTR_MASK             0x1FFFFF

/** @brief Host IPC Interrupt Enable mask in steered register block. */
#define MTL_IRQ_INTEN_L_HOST_IPC_MASK       (1 << 0)

/** @brief ACE Host IPC Interrupt Enable register (BAR4: 0x1140). */
#define MTL_DSP_REG_HfHIPCIE                0x1140

/** @brief Interrupt Enable bit mask for HfHIPCIE. */
#define MTL_DSP_REG_HfHIPCIE_IE_MASK        (1 << 0)

/** @brief Relative offset to DSP Interrupt Status register from steered pointer. */
#define MTL_DSP_IRQSTS                      0x20

/** @brief Mask indicating IPC event in IrqSts register. */
#define MTL_DSP_IRQSTS_IPC                  (1 << 0)

/* =========================================================================
 * Firmware / ROM Status Decoding Constants
 * ========================================================================= */

/** @brief Mask to isolate 24-bit state code from 32-bit FSR register. */
#define FSR_STATE_MASK                      0x00FFFFFF

/** @brief Helper macro to extract state code from raw FSR register value. */
#define FSR_TO_STATE_CODE(x)                ((x) & FSR_STATE_MASK)

/** @brief Firmware status: Initializing ROM hardware. */
#define FSR_STATE_INIT                      0x0

/** @brief Firmware status: ROM initialization complete, awaiting DMA download. */
#define FSR_STATE_INIT_DONE                 0x1

/** @brief Firmware status: Main FW entry point entered and initialized. */
#define FSR_STATE_FW_ENTERED                0x5

/**
 * @brief Retrieves the static hardware descriptor matching a PCI Vendor and Device ID.
 *
 * @param[in] VendorId PCI Vendor ID (e.g. PCI_VENDOR_ID_INTEL = 0x8086).
 * @param[in] DeviceId PCI Device ID (e.g. PCI_DEVICE_ID_INTEL_PTL = 0xE428).
 * @return Pointer to constant SOF_DSP_HW_DESC, or NULL if platform is unsupported.
 */
PSOF_DSP_HW_DESC SofGetHardwareDesc(
    _In_ USHORT VendorId,
    _In_ USHORT DeviceId
);

/* =========================================================================
 * DSP Core Dump & Exception Telemetry
 * ========================================================================= */

/** @brief Magic identifier for binary SOF core dump files: 'SOFD' (0x44464F53). */
#define SOF_COREDUMP_MAGIC                  0x44464F53

/** @brief Version number of the core dump file layout format. */
#define SOF_COREDUMP_VERSION                1

/**
 * @struct _SOF_DSP_COREDUMP_HEADER
 * @brief Binary header prepended to all DSP crash dump and telemetry files.
 *
 * Written to disk (e.g. C:\Windows\sof_dsp_coredump.bin) when an unhandled DSP
 * exception notification is received via IPC4 or an IPC doorbell times out.
 */
typedef struct _SOF_DSP_COREDUMP_HEADER {
    /** @brief Magic signature word ('SOFD'). */
    ULONG   Magic;

    /** @brief Format version number (SOF_COREDUMP_VERSION = 1). */
    ULONG   Version;

    /** @brief Total size in bytes of the dump file (Header + Mailbox payload). */
    ULONG   DumpSize;

    /** @brief System timestamp low DWORD (KeQuerySystemTime). */
    ULONG   TimestampLow;

    /** @brief System timestamp high DWORD. */
    ULONG   TimestampHigh;

    /** @brief PCI Vendor ID of the faulting DSP device. */
    USHORT  VendorId;

    /** @brief PCI Device ID of the faulting DSP device. */
    USHORT  DeviceId;

    /** @brief Reason code for the dump: 1=IPC4_EXCEPTION, 2=IPC_TIMEOUT, 3=DIAGNOSTIC. */
    ULONG   ReasonCode;

    /** @brief Human-readable reason string (up to 32 characters, null-padded). */
    CHAR    ReasonString[32];

    /** @brief Core 0 Status register snapshot (LNL_DSP_REG_HFDSC at 0x160200). */
    ULONG   Core0Status;

    /** @brief Core 0 Error register snapshot (LNL_DSP_REG_HFDEC at 0x160204). */
    ULONG   Core0Error;

    /** @brief Extended ROM status registers snapshot (8 DWORDs from 0x160200..0x16021C). */
    ULONG   ExtRomStatus[8];

    /** @brief Transmit Data register snapshot at fault. */
    ULONG   HipcTdr;

    /** @brief Transmit Ack register snapshot at fault. */
    ULONG   HipcTda;

    /** @brief Transmit Extension Data register snapshot at fault. */
    ULONG   HipcTdd;

    /** @brief Host Interrupt Data register snapshot at fault. */
    ULONG   HipcIdr;

    /** @brief Host Interrupt Ack register snapshot at fault. */
    ULONG   HipcIda;

    /** @brief Host Interrupt Extension Data register snapshot at fault. */
    ULONG   HipcIdd;

    /** @brief IPC Control register snapshot at fault. */
    ULONG   HipcCtl;

    /** @brief Uplink Mailbox offset relative to BAR4. */
    ULONG   UplinkMailboxOffset;

    /** @brief Size of captured Uplink Mailbox data (typically 4096 bytes). */
    ULONG   UplinkMailboxSize;

    /** @brief Downlink Mailbox offset relative to BAR4. */
    ULONG   DownlinkMailboxOffset;

    /** @brief Size of captured Downlink Mailbox data (typically 4096 bytes). */
    ULONG   DownlinkMailboxSize;

    /** @brief Debug Window offset relative to BAR4 (0 if not captured). */
    ULONG   DebugWindowOffset;

    /** @brief Size of captured Debug Window data (typically 65536 bytes). */
    ULONG   DebugWindowSize;

} SOF_DSP_COREDUMP_HEADER, *PSOF_DSP_COREDUMP_HEADER;

struct _SOF_DEVICE_CONTEXT;

/**
 * @brief Captures a binary core dump of DSP MMIO state, registers, and mailboxes.
 *
 * @param[in] Ctx Pointer to the device extension context.
 * @param[in] Reason Explanatory string describing the trigger event.
 */
VOID SofDspDumpCore(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ const char *Reason
);

/* =========================================================================
 * Helper Inline Functions for DSP MMIO Access (BAR4)
 * ========================================================================= */

/**
 * @brief Reads an 8-bit byte from the DSP MMIO space in BAR4.
 *
 * @param[in] Bar4Base Virtual base address mapping BAR4.
 * @param[in] Offset Byte offset within BAR4 to read.
 * @return 8-bit byte value read from hardware.
 */
static inline UCHAR SofDspRead8(
    _In_ PVOID Bar4Base,
    _In_ ULONG Offset
)
{
    return readb((PUCHAR)Bar4Base + Offset);
}

/**
 * @brief Reads a 32-bit register from the DSP MMIO space in BAR4.
 *
 * @param[in] Bar4Base Virtual base address mapping BAR4.
 * @param[in] Offset Byte offset within BAR4 to read.
 * @return 32-bit register value read from hardware.
 */
static inline ULONG SofDspRead32(
    _In_ PVOID Bar4Base,
    _In_ ULONG Offset
)
{
    return readl((PUCHAR)Bar4Base + Offset);
}

/**
 * @brief Writes a 32-bit value to a DSP MMIO register in BAR4.
 *
 * @param[in] Bar4Base Virtual base address mapping BAR4.
 * @param[in] Offset Byte offset within BAR4 to write.
 * @param[in] Value 32-bit value to write into hardware register.
 */
static inline VOID SofDspWrite32(
    _In_ PVOID Bar4Base,
    _In_ ULONG Offset,
    _In_ ULONG Value
)
{
    writel(Value, (PUCHAR)Bar4Base + Offset);
}

/**
 * @brief Performs an atomic read-modify-write operation on a 32-bit DSP MMIO register.
 *
 * @param[in] Bar4Base Virtual base address mapping BAR4.
 * @param[in] Offset Byte offset within BAR4.
 * @param[in] Mask Bitmask of bits to modify.
 * @param[in] Value New bit values to set within the masked positions.
 */
static inline VOID SofDspUpdateBits(
    _In_ PVOID Bar4Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value
)
{
    ULONG oldVal = SofDspRead32(Bar4Base, Offset);
    ULONG newVal = (oldVal & ~Mask) | (Value & Mask);
    SofDspWrite32(Bar4Base, Offset, newVal);
}

#ifdef __cplusplus
}
#endif

#endif /* __SOF_HW_H__ */
