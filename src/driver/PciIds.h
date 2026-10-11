/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file PciIds.h
 * @brief PCI Vendor and Device Identifiers for Sound Open Firmware (SOF) Hardware Platforms.
 *
 * This header defines PCI Vendor IDs and Device IDs for Intel and AMD audio DSPs
 * supported by the SOF Windows driver, covering Intel cAVS (Converged Audio Voice Speech),
 * Intel ACE (Advanced DSP Core Engine), and AMD ACP (Audio Co-Processor) architectures.
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#ifndef __SOF_PCI_IDS_H__
#define __SOF_PCI_IDS_H__

/* =========================================================================
 * PCI Vendors
 * ========================================================================= */

/** @brief PCI Vendor ID for Intel Corporation. */
#define PCI_VENDOR_ID_INTEL             0x8086

/** @brief PCI Vendor ID for Advanced Micro Devices, Inc. */
#define PCI_VENDOR_ID_AMD               0x1022

/* =========================================================================
 * Intel ACE Architecture (Advanced DSP Core Engine)
 *
 * ACE is Intel's next-generation DSP microarchitecture, featuring dedicated
 * DSP power domains (WPDSPHPXPG), hardware-managed memory retention,
 * direct mailbox windows in BAR4, and the HFIPCX doorbell interface.
 * ========================================================================= */

/** @brief Intel Panther Lake Mobile (PTL-U/H/P) - ACE 3.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_PTL         0xE428

/** @brief Intel Panther Lake High Performance (PTL-H) - ACE 3.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_PTL_H       0x4F80

/** @brief Intel Panther Lake High Performance Alternate (PTL-H) - ACE 3.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_PTL_H_ALT   0xE328

/** @brief Intel Wildcat Lake Mobile (WCL) - ACE 3.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_WCL         0x4D28

/** @brief Intel Nova Lake Mobile (NVL) - ACE 3.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_NVL         0xD328

/** @brief Intel Nova Lake Desktop (NVL-S) - ACE 3.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_NVL_S       0x6E50

/** @brief Intel Lunar Lake Mobile (LNL-M) - ACE 2.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_LNL         0xA828

/** @brief Intel Meteor Lake Mobile (MTL-P/M) - ACE 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_MTL         0x7E28

/** @brief Intel Meteor Lake Desktop (MTL-S) - ACE 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_MTL_S       0x7728

/** @brief Intel Arrow Lake Mobile (ARL-H) - ACE 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ARL         0x7F50

/** @brief Intel Arrow Lake Desktop (ARL-S) - ACE 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ARL_S       0x7E50

/* =========================================================================
 * Intel cAVS Architecture (Converged Audio Voice Speech)
 *
 * cAVS is Intel's audio subsystem spanning Core 6th Gen through 14th Gen.
 * Uses ADSPCS power/clock gating, legacy CNL/TGL doorbells, and classic
 * SRAM memory windows 0 and 1.
 * ========================================================================= */

/** @brief Intel Alder Lake Desktop (ADL-S) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ADL_S       0x7AD0

/** @brief Intel Alder Lake Mobile (ADL-P) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ADL_P       0x51C8

/** @brief Intel Alder Lake High Performance Mobile (ADL-PS) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ADL_PS      0x51C9

/** @brief Intel Alder Lake Ultra Mobile (ADL-M) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ADL_M       0x51CC

/** @brief Intel Alder Lake Extreme Mobile (ADL-PX) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ADL_PX      0x51CD

/** @brief Intel Alder Lake Entry / N-Series (ADL-N) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ADL_N       0x54C8

/** @brief Intel Raptor Lake Desktop (RPL-S) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_RPL_S       0x7A50

/** @brief Intel Raptor Lake Mobile (RPL-P) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_RPL_P       0x51CA

/** @brief Intel Raptor Lake Mobile Variant 1 (RPL-P-1) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_RPL_P_1     0x51CB

/** @brief Intel Raptor Lake Ultra Mobile (RPL-M) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_RPL_M       0x51CE

/** @brief Intel Raptor Lake Extreme Mobile (RPL-PX) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_RPL_PX      0x51CF

/** @brief Intel Rocket Lake Desktop (RKL-S) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_RKL_S       0xF1C8

/** @brief Intel Tiger Lake Mobile (TGL-LP) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_TGL         0xA0C8

/** @brief Intel Tiger Lake High Performance Mobile (TGL-H) - cAVS 2.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_TGL_H       0x43C8

/** @brief Intel Jasper Lake (JSL) - cAVS 2.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_JSL         0x4B55

/** @brief Intel Jasper Lake-N (JSL-N) - cAVS 2.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_JSL_N       0x4DC8

/** @brief Intel Elkhart Lake (EHL) - cAVS 2.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_EHL         0x4B58

/** @brief Intel Ice Lake Mobile (ICL-LP) - cAVS 2.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ICL         0x34C8

/** @brief Intel Ice Lake High Performance (ICL-H) - cAVS 2.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ICL_H       0x3DC8

/** @brief Intel Ice Lake-N (ICL-N) - cAVS 2.0 Audio DSP */
#define PCI_DEVICE_ID_INTEL_ICL_N       0x38C8

/** @brief Intel Cannon Lake Mobile (CNL-LP) - cAVS 1.8 Audio DSP */
#define PCI_DEVICE_ID_INTEL_CNL         0x9DC8

/** @brief Intel Cannon Lake / Coffee Lake High Performance (CNL-H) - cAVS 1.8 Audio DSP */
#define PCI_DEVICE_ID_INTEL_CNL_H       0xA348

/** @brief Intel Comet Lake Low Power (CML-LP) - cAVS 1.8 Audio DSP */
#define PCI_DEVICE_ID_INTEL_CML_LP      0x02C8

/** @brief Intel Comet Lake High Performance (CML-H) - cAVS 1.8 Audio DSP */
#define PCI_DEVICE_ID_INTEL_CML_H       0x06C8

/** @brief Intel Comet Lake Desktop (CML-S) - cAVS 1.8 Audio DSP */
#define PCI_DEVICE_ID_INTEL_CML_S       0xA3F0

/** @brief Intel Comet Lake-R (CML-R) - cAVS 1.8 Audio DSP */
#define PCI_DEVICE_ID_INTEL_CML_R       0xF0C8

/** @brief Intel Apollo Lake (APL) - cAVS 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_APL         0x5A98

/** @brief Intel Gemini Lake (GLK) - cAVS 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_GLK         0x3198

/** @brief Intel Skylake Low Power (SKL-LP) - cAVS 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_SKL_LP      0x9D70

/** @brief Intel Skylake High Performance (SKL-H) - cAVS 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_SKL_H       0xA170

/** @brief Intel Kaby Lake Low Power (KBL-LP) - cAVS 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_KBL_LP      0x9D71

/** @brief Intel Kaby Lake High Performance (KBL-H) - cAVS 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_KBL_H       0xA171

/** @brief Intel Kaby Lake High Performance Alternate (KBL-H) - cAVS 1.5 Audio DSP */
#define PCI_DEVICE_ID_INTEL_KBL_H_ALT   0xA2F0

/* =========================================================================
 * AMD ACP Architecture (Audio Co-Processor)
 *
 * AMD Audio Co-Processor subsystems integrated into Ryzen APUs.
 * ========================================================================= */

/** @brief AMD Strix Point / Kraken Point (Ryzen AI 300) - ACP 7.0 / 7.x Audio DSP */
#define PCI_DEVICE_ID_AMD_ACP_7_0       0x1502

/** @brief AMD Phoenix / Hawk Point (Ryzen 7040 / 8040) - ACP 6.3 Audio DSP */
#define PCI_DEVICE_ID_AMD_ACP_6_3       0x15E4

/** @brief AMD Rembrandt / Mendocino (Ryzen 6000 / 7020) - ACP 6.0 Audio DSP */
#define PCI_DEVICE_ID_AMD_ACP_RMB       0x15E3

/** @brief AMD VanGogh (Valve Steam Deck Custom APU) - ACP Audio DSP */
#define PCI_DEVICE_ID_AMD_ACP_VANGOGH   0x1640

/** @brief AMD Renoir / Cezanne / Barcelo (Ryzen 4000 / 5000) - ACP 3.x Audio DSP */
#define PCI_DEVICE_ID_AMD_ACP_RN        0x15E2

#endif /* __SOF_PCI_IDS_H__ */
