/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file SdwController.h
 * @brief SoundWire Controller Header - Cadence Master IP, Intel SHIM2 & Microsoft SDCA DDI.
 *
 * @details
 * Declares register offsets, bitfields, context structures, and function prototypes
 * for operating the Cadence SoundWire Master IP, Intel SHIM2 vendor-specific registers,
 * and enumerating child SDCA (SoundWire Device Class Audio) peripheral devices on Intel
 * Panther Lake, Lunar Lake, and Meteor Lake platforms.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_SDW_CONTROLLER_H__
#define __SOF_SDW_CONTROLLER_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <SoundWireController.h>
#else
#include <windows.h>
#include <strsafe.h>
#define RtlStringCbPrintfA StringCbPrintfA
#include <ks.h>
#include <devioctl.h>
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_DEVICE_NOT_READY
#define STATUS_DEVICE_NOT_READY          ((NTSTATUS)0xC000010AL)
#endif
#ifndef STATUS_IO_TIMEOUT
#define STATUS_IO_TIMEOUT                ((NTSTATUS)0xC00000B5L)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009AL)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#endif
#endif

#include <sof/sof_osal.h>
#include "Topo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of device context */
typedef struct _SOF_DEVICE_CONTEXT SOF_DEVICE_CONTEXT, *PSOF_DEVICE_CONTEXT;

/* =========================================================================
 * SoundWire BAR0 Base Offsets on Intel Panther Lake (ACE 3.0 / ACE 2.x)
 * Authoritative reference: drivers/soundwire/intel_ace2x.c & sdw_intel.h
 * ========================================================================= */
#define SOF_SDW_SHIM_BASE_ACE               0x38000 /**< Base offset of SoundWire SHIM on ACE */
#define SOF_SDW_ALH_BASE_ACE                0x24000 /**< Audio Link Hub (ALH) base on ACE */
#define SOF_SDW_SHIM2_GENERIC_BASE(link)    (0x30000 + (0x8000 * (link))) /**< Generic SHIM2 base per link */
#define SOF_SDW_IP_BASE(link)               (0x30100 + (0x8000 * (link))) /**< Cadence Master IP base per link */
#define SOF_SDW_SHIM2_VS_BASE(link)         (0x36000 + (0x8000 * (link))) /**< Vendor-Specific SHIM2 base per link */
#define SOF_SDW_MAX_LINKS                   4       /**< Maximum supported SoundWire master links */
#define SOF_SDW_MAX_CHILDREN                8       /**< Maximum child peripheral devices tracked */

/* =========================================================================
 * Cadence Master IP Register Offsets & Bitfields
 * Authoritative reference: drivers/soundwire/cadence_master.c
 * ========================================================================= */
#define CDNS_MCP_CONFIG                     0x0000  /**< Master Control Protocol Configuration */
#define CDNS_MCP_CONTROL                    0x0004  /**< Master Control (resets, soft reset) */
#define CDNS_MCP_CMD                        0x0008  /**< Master Command trigger */
#define CDNS_MCP_PHYCTRL                    0x0010  /**< PHY Control */
#define CDNS_MCP_SSP_CTRL0                  0x0020  /**< Sample Sub-Period Control 0 */
#define CDNS_MCP_SSP_CTRL1                  0x0024  /**< Sample Sub-Period Control 1 */
#define CDNS_MCP_CLK_CTRL0                  0x0030  /**< Clock Control 0 */
#define CDNS_MCP_CLK_CTRL1                  0x0034  /**< Clock Control 1 */
#define CDNS_MCP_FRAME_SHAPE_INIT           0x003C  /**< Initial Frame Shape (rows, cols) */
#define CDNS_MCP_FIFOSTAT                   0x0044  /**< Command/Response FIFO Status */
#define CDNS_MCP_CONFIG_UPDATE              0x0048  /**< Configuration Update strobe */
#define CDNS_MCP_FIFOLEVEL                  0x004C  /**< Command FIFO Level */
#define CDNS_MCP_CMD_BASE                   0x0080  /**< Command FIFO Data Register */
#define CDNS_MCP_RESP_BASE                  0x0080  /**< Response FIFO Data Register */

/* Cadence MCP Control Bits */
#define CDNS_MCP_CONTROL_HW_RST             (1 << 5) /**< Hardware Reset */
#define CDNS_MCP_CONTROL_SW_RST             (1 << 6) /**< Software Reset */
#define CDNS_MCP_CONTROL_CMD_RST            (1 << 7) /**< Command Engine Reset */

/* Cadence MCP Config Update Bits */
#define CDNS_MCP_CONFIG_UPDATE_BIT          (1 << 0) /**< Strobe configuration update to shadow registers */

/* Cadence MCP FIFO Status Bits */
#define CDNS_MCP_FIFOSTAT_RX_AVAIL          (1 << 0) /**< Receive response available in FIFO */
#define CDNS_MCP_FIFOSTAT_TX_EMPTY          (1 << 1) /**< Transmit command FIFO is empty */

/* =========================================================================
 * Intel SHIM2 Vendor-Specific Registers
 * Authoritative reference: drivers/soundwire/intel_ace2x.c
 * ========================================================================= */
#define SDW_SHIM2_INTEL_VS_LVSCTL           0x0004  /**< Link Vendor Specific Control */
#define SDW_SHIM2_MLCS_XTAL_CLK             0x0     /**< Clock source: Crystal oscillator */
#define SDW_SHIM2_MLCS_AUDIO_PLL_CLK        0x2     /**< Clock source: Audio PLL */
#define SDW_SHIM2_INTEL_VS_ACTMCTL          0x000E  /**< Autonomous Clock Transition Mode Control */
#define SDW_SHIM2_INTEL_VS_ACTMCTL_DACTQE   (1 << 0) /**< Dynamic Autonomous Clock Transition Queue Enable */

/* =========================================================================
 * MIPI SoundWire Standard Peripheral Registers
 * ========================================================================= */
#define SDW_SCP_DEVNUMBER                   0x0050  /**< Assigned Device Number */
#define SDW_SCP_DEVID_0                     0x0051  /**< Device ID Byte 0 */
#define SDW_SCP_DEVID_1                     0x0052  /**< Device ID Byte 1 */
#define SDW_SCP_DEVID_2                     0x0053  /**< Device ID Byte 2 */
#define SDW_SCP_DEVID_3                     0x0054  /**< Device ID Byte 3 */
#define SDW_SCP_DEVID_4                     0x0055  /**< Device ID Byte 4 */
#define SDW_SCP_DEVID_5                     0x0056  /**< Device ID Byte 5 */
#define SDW_SCP_FRAMESHAPE                  0x0057  /**< Active Frame Shape */
#define SDW_SCP_SECFRAMESHAPE               0x0058  /**< Secondary Frame Shape */

/* =========================================================================
 * SDCA Audio Entity & Control Selectors (SDCA v1.0 / v1.1)
 * ========================================================================= */
#define SDCA_FUNCTION_TYPE_AUDIO            0x01    /**< Standard Audio Function */
#define SDCA_CONTROL_ENTITY_0               0x00    /**< Control Entity 0 (Device Management) */
#define SDCA_CONTROL_ENTITY_IT              0x01    /**< Input Terminal */
#define SDCA_CONTROL_ENTITY_FU              0x02    /**< Feature Unit (Volume/Mute) */
#define SDCA_CONTROL_ENTITY_OT              0x03    /**< Output Terminal */

#ifdef _KERNEL_MODE
/**
 * @struct _SOF_SDW_CHILD_CONTEXT
 * @brief Context structure attached to each child PDO representing a SoundWire SDCA Peripheral.
 */
typedef struct _SOF_SDW_CHILD_CONTEXT {
    PSOF_DEVICE_CONTEXT                 ParentCtx;          /**< Pointer to parent SOF device context */
    ULONG                               LinkIndex;          /**< SoundWire master link index (0..3) */
    ULONG                               PeripheralId;       /**< Assigned peripheral address (1..11) */
    ULONG                               UniqueId;           /**< Discovered unique instance ID */
    ULONG                               VendorId;           /**< MIPI Vendor ID (e.g. 0x025D Realtek) */
    ULONG                               DeviceId;           /**< Peripheral Part/Device ID */
    ULONG                               DeviceType;         /**< SDCA Device Type */
    ULONG                               Direction;          /**< 0 = Render, 1 = Capture */
    ULONG                               DataPortNumber;     /**< SoundWire Data Port used for streaming */
    BOOLEAN                             IsPrepared;         /**< TRUE if data port is prepared */
    BOOLEAN                             IsStarted;          /**< TRUE if data port is running */
    SOUNDWIRE_DATAPORT_CONFIGURATION_2  DataPortConfig;     /**< Cached DDI port configuration */
    PFNSOUNDWIRE_NOTIFICATION_CALLBACK  NotificationCallback; /**< Registered notification callback */
    PVOID                               NotificationContext; /**< Context passed to notification callback */
    PFNSOUNDWIRE_INTERRUPT_CALLBACK     InterruptCallback;  /**< Registered interrupt callback */
    PVOID                               InterruptContext;   /**< Context passed to interrupt callback */
    ULONG                               NotificationMask;   /**< Mask of monitored notifications */
    ULONG                               InterruptMask;      /**< Mask of monitored interrupts */
    ULONG                               ClockRefCount;      /**< Link clock reference count */
    LONG                                CachedVolumeDb;     /**< Cached volume setting */
    BOOLEAN                             CachedMute;         /**< Cached mute setting */
} SOF_SDW_CHILD_CONTEXT, *PSOF_SDW_CHILD_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SOF_SDW_CHILD_CONTEXT, SofSdwGetChildContext)
#endif

/*
 * SoundWire Controller & Child Management Prototypes
 */

/**
 * @brief Initializes the SoundWire Master controller hardware on Intel ACE.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofSdwInitController(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Tears down the SoundWire controller hardware and releases child PDOs.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 */
VOID SofSdwUninitController(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Discovers and enumerates child SoundWire peripheral devices based on NHLT tables.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofSdwEnumerateChildren(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Transmits a SoundWire control register read or write across Cadence Master IP.
 *
 * @param[in]     Ctx       Pointer to SOF device context.
 * @param[in]     LinkIndex SoundWire master link index (0..3).
 * @param[in]     DevAddr   Peripheral device address (0..14).
 * @param[in]     RegAddr   16-bit register address.
 * @param[in,out] Data      Pointer to read buffer or write data.
 * @param[in]     IsWrite   TRUE for write; FALSE for read.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofSdwTransferMsg(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG LinkIndex,
    _In_ UCHAR DevAddr,
    _In_ USHORT RegAddr,
    _Inout_ PUCHAR Data,
    _In_ BOOLEAN IsWrite
);

/**
 * @brief Synthesizes register reads for peripheral enumeration when hardware is in bypass.
 *
 * @param[in] LinkIndex Master link index.
 * @param[in] DeviceId  Target device identifier.
 * @param[in] RegAddr   Register address queried.
 * @return Synthesized register byte value.
 */
UCHAR SofSdwSynthesizeRegRead(
    _In_ ULONG LinkIndex,
    _In_ ULONG DeviceId,
    _In_ ULONG RegAddr
);

#ifdef _KERNEL_MODE
/**
 * @brief WDF IOCTL dispatch callback for SoundWire child PDOs handling SDCA DDI requests.
 *
 * @param[in] Queue         WDF IO queue handle.
 * @param[in] Request       WDF request handle.
 * @param[in] OutputBufferLength Output buffer length.
 * @param[in] InputBufferLength  Input buffer length.
 * @param[in] IoControlCode      IOCTL control code.
 */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SofSdwEvtIoDeviceControl;
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SOF_SDW_CONTROLLER_H__ */
