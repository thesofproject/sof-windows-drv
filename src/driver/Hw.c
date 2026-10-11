/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Hw.c
 * @brief Hardware Descriptor Tables and Platform Lookup Engine for Intel cAVS, ACE, and AMD DSPs.
 *
 * Implements hardware capability descriptors and dynamic platform resolution by
 * matching PCI Vendor and Device IDs against known Intel and AMD microarchitectural
 * generations.
 *
 * Microarchitecture Memory Map Summary:
 * - cAVS 1.5 - 2.5:
 *   SRAM Window 0 (FW Status / Uplink):  BAR4 + 0x80000 (Status at 0x80000, Uplink at 0x81000)
 *   SRAM Window 1 (Downlink Mailbox):    BAR4 + 0xA0000 (4KB)
 *   Doorbells (HIPC):                   BAR4 + 0x00C0 (CNL/TGL) or 0x0040 (APL)
 * - ACE 1.0 (Meteor Lake / Arrow Lake):
 *   SRAM Window 0 (ROM Status):         BAR4 + 0x180000 (Status at 0x180000, Uplink at 0x181000)
 *   SRAM Window 1 (Downlink Mailbox):    BAR4 + 0x188000 (4KB)
 *   Doorbells (HFIPCX):                 BAR4 + 0x73200 (HIPCXTDR/HIPCXIDR)
 *   Interrupt Steering:                 BAR4 + 0x1108 (MTL_HFINTIPPTR)
 * - ACE 2.0 / 3.0 (Lunar Lake / Panther Lake):
 *   Extended ROM Status:                BAR4 + 0x160200 (LNL_DSP_REG_HFDSC)
 *   SRAM Windows:                       Uplink at 0x181000, Downlink at 0x188000
 *   Power Rails:                        HFPWRCTL (0x1D18) & HFPWRCTL2 (0x1D20)
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#include "Hw.h"

/* =========================================================================
 * Hardware Descriptors
 * ========================================================================= */

/**
 * @brief Static descriptor for Intel cAVS 2.5 platforms.
 *
 * Encompasses Tiger Lake (TGL), Alder Lake (ADL), and Raptor Lake (RPL).
 * Features 4 DSP cores, IPC4 firmware protocol support, 40h doorbell base,
 * and classic memory windows at 0x80000 / 0xA0000.
 */
static SOF_DSP_HW_DESC g_IntelCavs25Desc = {
    SOF_HW_INTEL_CAVS_2_5,
    "Intel cAVS 2.5 (TGL/ADL/RPL)",
    4,                      /* CoresNum: 4 Xtensa DSP cores */
    0x80000,                /* RomStatusReg: Window 0 base offset */
    0x80004,                /* RomErrorReg: Window 0 + 4 */
    0x80004,                /* FwStatusReg: Live FW status reported here */
    0xa0000,                /* MailboxDownlinkOffset: Window 1 (Host -> DSP payload) */
    0x1000,                 /* MailboxDownlinkSize: 4096 bytes */
    0x81000,                /* MailboxUplinkOffset: Window 0 + 0x1000 (DSP -> Host payload) */
    0x1000,                 /* MailboxUplinkSize: 4096 bytes */
    0x00c0,                 /* HipcTdr: CNL_DSP_REG_HIPCTDR (DSP -> Host doorbell) */
    0x00c4,                 /* HipcTda: CNL_DSP_REG_HIPCTDA (DSP -> Host ACK) */
    0x00c8,                 /* HipcTdd: CNL_DSP_REG_HIPCTDD (DSP -> Host extension) */
    0x00d0,                 /* HipcIdr: CNL_DSP_REG_HIPCIDR (Host -> DSP doorbell) */
    0x00d4,                 /* HipcIda: CNL_DSP_REG_HIPCIDA (Host -> DSP ACK) */
    0x00d8,                 /* HipcIdd: CNL_DSP_REG_HIPCIDD (Host -> DSP extension) */
    0x00e8,                 /* HipcCtl: CNL_DSP_REG_HIPCCTL (IPC interrupt mask) */
    0x80000000,             /* HipcTdrBusy: Bit 31 asserted by DSP when transmitting */
    0x80000000,             /* HipcTdaDone: Bit 31 asserted by Host when clearing */
    0x80000000,             /* HipcIdrBusy: Bit 31 asserted by Host when ringing doorbell */
    0x80000000,             /* HipcIdaDone: Bit 31 asserted by DSP upon command completion */
    0x00000001,             /* HipcCtlBusy: Bit 0 enables Busy interrupt */
    0x00000002,             /* HipcCtlDone: Bit 1 enables Done interrupt */
    FALSE,                  /* IsAce: Legacy cAVS interrupt routing */
    0, 0, 0                 /* ACE IRQ params unused on cAVS */
};

/**
 * @brief Static descriptor for Intel cAVS 2.0 platforms.
 *
 * Encompasses Ice Lake (ICL), Jasper Lake (JSL), and Elkhart Lake (EHL).
 */
static SOF_DSP_HW_DESC g_IntelCavs20Desc = {
    SOF_HW_INTEL_CAVS_2_0,
    "Intel cAVS 2.0 (ICL/JSL/EHL)",
    4,
    0x80000, 0x80004, 0x80004,
    0xa0000, 0x1000,
    0x81000, 0x1000,
    0x00c0, 0x00c4, 0x00c8, 0x00d0, 0x00d4, 0x00d8, 0x00e8,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0x00000001, 0x00000002,
    FALSE,
    0, 0, 0
};

/**
 * @brief Static descriptor for Intel cAVS 1.8 platforms.
 *
 * Encompasses Cannon Lake (CNL) and Comet Lake (CML).
 */
static SOF_DSP_HW_DESC g_IntelCavs18Desc = {
    SOF_HW_INTEL_CAVS_1_8,
    "Intel cAVS 1.8 (CNL/CML)",
    4,
    0x80000, 0x80004, 0x80004,
    0xa0000, 0x1000,
    0x81000, 0x1000,
    0x00c0, 0x00c4, 0x00c8, 0x00d0, 0x00d4, 0x00d8, 0x00e8,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0x00000001, 0x00000002,
    FALSE,
    0, 0, 0
};

/**
 * @brief Static descriptor for Intel cAVS 1.5 platforms.
 *
 * Encompasses Apollo Lake (APL) and Gemini Lake (GLK). Features 2 DSP cores
 * and earlier 0x40 doorbell register base.
 */
static SOF_DSP_HW_DESC g_IntelCavs15Desc = {
    SOF_HW_INTEL_CAVS_1_5,
    "Intel cAVS 1.5 (APL/GLK)",
    2,
    0x80000, 0x80004, 0x80004,
    0xa0000, 0x1000,
    0x81000, 0x1000,
    0x0040, 0x0044, 0x0044, 0x0048, 0x004c, 0x004c, 0x0050,
    0x80000000, 0x40000000, 0x80000000, 0x40000000,
    0x00000001, 0x00000002,
    FALSE,
    0, 0, 0
};

/**
 * @brief Static descriptor for Intel ACE 1.0 / 1.5 platforms.
 *
 * Encompasses Meteor Lake (MTL) and Arrow Lake (ARL).
 * Introduces high-performance DSP subsystem power control (HFDSSCS),
 * relocated SRAM mailbox windows (0x180000 / 0x188000), 0x73200 HFIPCX doorbells,
 * and indirect interrupt steering via MTL_HFINTIPPTR.
 */
static SOF_DSP_HW_DESC g_IntelAce10Desc = {
    SOF_HW_INTEL_ACE_1_0,
    "Intel ACE 1.0 (MTL/ARL)",
    4,                      /* CoresNum: 4 High-Performance DSP cores */
    0x180000,               /* RomStatusReg: MTL_DSP_ROM_STS */
    0x180004,               /* RomErrorReg: MTL_DSP_ROM_ERROR */
    0x180004,               /* FwStatusReg */
    0x188000,               /* MailboxDownlinkOffset: MTL_DSP_MBOX_DOWNLINK_OFFSET */
    0x1000,                 /* MailboxDownlinkSize: 4096 bytes */
    0x181000,               /* MailboxUplinkOffset: MTL_DSP_MBOX_UPLINK_OFFSET */
    0x1000,                 /* MailboxUplinkSize: 4096 bytes */
    0x73200,                /* HipcTdr: MTL_DSP_REG_HFIPCXTDR */
    0x73204,                /* HipcTda: MTL_DSP_REG_HFIPCXTDA */
    0x73300,                /* HipcTdd: MTL_DSP_REG_HFIPCXTDDY */
    0x73210,                /* HipcIdr: MTL_DSP_REG_HFIPCXIDR */
    0x73214,                /* HipcIda: MTL_DSP_REG_HFIPCXIDA */
    0x73380,                /* HipcIdd: MTL_DSP_REG_HFIPCXIDDY */
    0x73228,                /* HipcCtl: MTL_DSP_REG_HFIPCXCTL */
    0x80000000,             /* HipcTdrBusy: Bit 31 */
    0x80000000,             /* HipcTdaDone: Bit 31 */
    0x80000000,             /* HipcIdrBusy: Bit 31 */
    0x80000000,             /* HipcIdaDone: Bit 31 */
    0x00000001,             /* HipcCtlBusy: Bit 0 */
    0x00000002,             /* HipcCtlDone: Bit 1 */
    TRUE,                   /* IsAce: Uses ACE interrupt pointer */
    0x1108,                 /* IntIpPtrReg: MTL_HFINTIPPTR */
    0x0020,                 /* IrqStsOffset: MTL_DSP_IRQSTS relative to ptr */
    0x00000001              /* IrqStsIpcMask: MTL_DSP_IRQSTS_IPC (BIT 0) */
};

/**
 * @brief Static descriptor for Intel ACE 2.0 platforms.
 *
 * Encompasses Lunar Lake (LNL). Features 5 DSP cores and relocated extended
 * ROM status registers at 0x160200 (LNL_DSP_REG_HFDSC).
 */
static SOF_DSP_HW_DESC g_IntelAce20Desc = {
    SOF_HW_INTEL_ACE_2_0,
    "Intel ACE 2.0 (LNL)",
    5,                      /* CoresNum: 5 DSP cores */
    0x160200, 0x160204, 0x160200,
    0x188000, 0x1000,
    0x181000, 0x1000,
    0x73200, 0x73204, 0x73300, 0x73210, 0x73214, 0x73380, 0x73228,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0x00000001, 0x00000002,
    TRUE,
    0x1108, 0x0020, 0x00000001
};

/**
 * @brief Static descriptor for Intel ACE 3.0 platforms.
 *
 * Encompasses Panther Lake (PTL). Features 5 DSP cores, dual HFPWRCTL2 power
 * gating registers, integrated USB Audio Offload (UAOL), and 0x160200 status registers.
 */
static SOF_DSP_HW_DESC g_IntelAce30Desc = {
    SOF_HW_INTEL_ACE_3_0,
    "Intel ACE 3.0 (PTL)",
    5,                      /* CoresNum: 5 DSP cores */
    0x160200, 0x160204, 0x160200,
    0x188000, 0x1000,
    0x181000, 0x1000,
    0x73200, 0x73204, 0x73300, 0x73210, 0x73214, 0x73380, 0x73228,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0x00000001, 0x00000002,
    TRUE,
    0x1108, 0x0020, 0x00000001
};

/**
 * @brief Static descriptor for Intel ACE 4.0 platforms.
 *
 * Encompasses Nova Lake Mobile (NVL). Features 4 DSP cores, ACE 4.0 power
 * gating architecture, PTL-derived hardware ops, and 0x160200 status registers.
 */
static SOF_DSP_HW_DESC g_IntelAce40Desc = {
    SOF_HW_INTEL_ACE_4_0,
    "Intel ACE 4.0 (NVL)",
    4,                      /* CoresNum: 4 DSP cores */
    0x160200, 0x160204, 0x160200,
    0x188000, 0x1000,
    0x181000, 0x1000,
    0x73200, 0x73204, 0x73300, 0x73210, 0x73214, 0x73380, 0x73228,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0x00000001, 0x00000002,
    TRUE,
    0x1108, 0x0020, 0x00000001
};

/**
 * @brief Static descriptor for Intel ACE 4.0 Desktop platforms.
 *
 * Encompasses Nova Lake Desktop (NVL-S). Features 2 DSP cores.
 */
static SOF_DSP_HW_DESC g_IntelAce40SDesc = {
    SOF_HW_INTEL_ACE_4_0,
    "Intel ACE 4.0 (NVL-S)",
    2,                      /* CoresNum: 2 DSP cores */
    0x160200, 0x160204, 0x160200,
    0x188000, 0x1000,
    0x181000, 0x1000,
    0x73200, 0x73204, 0x73300, 0x73210, 0x73214, 0x73380, 0x73228,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0x00000001, 0x00000002,
    TRUE,
    0x1108, 0x0020, 0x00000001
};

/**
 * @brief Static descriptor for AMD Audio Co-Processor (ACP) platforms.
 */
static SOF_DSP_HW_DESC g_AmdAcpDesc = {
    SOF_HW_AMD_ACP,
    "AMD ACP Subsystem",
    2,
    0x0, 0x4, 0x4,
    0x1000, 0x1000,
    0x2000, 0x1000,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    FALSE,
    0, 0, 0
};

/* =========================================================================
 * Platform Lookup Implementation
 * ========================================================================= */

PSOF_DSP_HW_DESC SofGetHardwareDesc(
    _In_ USHORT VendorId,
    _In_ USHORT DeviceId
)
{
    if (VendorId == PCI_VENDOR_ID_INTEL) {
        switch (DeviceId) {
        /* Nova Lake (ACE 4.0) */
        case PCI_DEVICE_ID_INTEL_NVL:
            return &g_IntelAce40Desc;
        case PCI_DEVICE_ID_INTEL_NVL_S:
            return &g_IntelAce40SDesc;

        /* Panther Lake & Wildcat Lake (ACE 3.0) */
        case PCI_DEVICE_ID_INTEL_PTL:
        case PCI_DEVICE_ID_INTEL_PTL_H:
        case PCI_DEVICE_ID_INTEL_PTL_H_ALT:
        case PCI_DEVICE_ID_INTEL_WCL:
            return &g_IntelAce30Desc;

        /* Lunar Lake (ACE 2.0) */
        case PCI_DEVICE_ID_INTEL_LNL:
            return &g_IntelAce20Desc;

        /* Meteor Lake & Arrow Lake (ACE 1.0 / 1.5) */
        case PCI_DEVICE_ID_INTEL_MTL:
        case PCI_DEVICE_ID_INTEL_MTL_S:
        case PCI_DEVICE_ID_INTEL_ARL:
        case PCI_DEVICE_ID_INTEL_ARL_S:
            return &g_IntelAce10Desc;

        /* Raptor Lake, Alder Lake, Rocket Lake & Tiger Lake (cAVS 2.5) */
        case PCI_DEVICE_ID_INTEL_ADL_S:
        case PCI_DEVICE_ID_INTEL_ADL_P:
        case PCI_DEVICE_ID_INTEL_ADL_PS:
        case PCI_DEVICE_ID_INTEL_ADL_M:
        case PCI_DEVICE_ID_INTEL_ADL_PX:
        case PCI_DEVICE_ID_INTEL_ADL_N:
        case PCI_DEVICE_ID_INTEL_RPL_S:
        case PCI_DEVICE_ID_INTEL_RPL_P:
        case PCI_DEVICE_ID_INTEL_RPL_P_1:
        case PCI_DEVICE_ID_INTEL_RPL_M:
        case PCI_DEVICE_ID_INTEL_RPL_PX:
        case PCI_DEVICE_ID_INTEL_RKL_S:
        case PCI_DEVICE_ID_INTEL_TGL:
        case PCI_DEVICE_ID_INTEL_TGL_H:
            return &g_IntelCavs25Desc;

        /* Ice Lake, Jasper Lake & Elkhart Lake (cAVS 2.0) */
        case PCI_DEVICE_ID_INTEL_ICL:
        case PCI_DEVICE_ID_INTEL_ICL_H:
        case PCI_DEVICE_ID_INTEL_ICL_N:
        case PCI_DEVICE_ID_INTEL_JSL:
        case PCI_DEVICE_ID_INTEL_JSL_N:
        case PCI_DEVICE_ID_INTEL_EHL:
            return &g_IntelCavs20Desc;

        /* Cannon Lake, Comet Lake & Coffee Lake (cAVS 1.8) */
        case PCI_DEVICE_ID_INTEL_CNL:
        case PCI_DEVICE_ID_INTEL_CNL_H:
        case PCI_DEVICE_ID_INTEL_CML_LP:
        case PCI_DEVICE_ID_INTEL_CML_H:
        case PCI_DEVICE_ID_INTEL_CML_S:
        case PCI_DEVICE_ID_INTEL_CML_R:
            return &g_IntelCavs18Desc;

        /* Apollo Lake, Gemini Lake, Skylake & Kaby Lake (cAVS 1.5) */
        case PCI_DEVICE_ID_INTEL_APL:
        case PCI_DEVICE_ID_INTEL_GLK:
        case PCI_DEVICE_ID_INTEL_SKL_LP:
        case PCI_DEVICE_ID_INTEL_SKL_H:
        case PCI_DEVICE_ID_INTEL_KBL_LP:
        case PCI_DEVICE_ID_INTEL_KBL_H:
        case PCI_DEVICE_ID_INTEL_KBL_H_ALT:
            return &g_IntelCavs15Desc;

        default:
            /* Default to cAVS 2.5 descriptor for modern unrecognized Intel DSPs */
            return &g_IntelCavs25Desc;
        }
    } else if (VendorId == PCI_VENDOR_ID_AMD) {
        return &g_AmdAcpDesc;
    }

    return NULL;
}
