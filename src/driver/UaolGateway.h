/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file UaolGateway.h
 * @brief Sound Open Firmware (SOF) Windows Driver - UAOL Gateway & Link DMA Header.
 *
 * @details
 * Declares data structures and API functions connecting Sound Open Firmware DSP
 * audio processing pipelines to the Intel USB Audio Offload Link (UAOL).
 *
 * Hardware Architecture:
 *  - Intel USB Audio Offload (UAOL): Hardware interconnection between the Intel cAVS/ACE
 *    Audio DSP and the Intel xHCI USB 3.x Host Controller.
 *  - Link DMA: Hardware stream engines in BAR0 programmed to pipe raw audio PCM frames
 *    between DSP SRAM and the xHCI sideband FIFO.
 *  - IPC4 Gateway Nodes:
 *    - SOF_IPC4_GATEWAY_UAOL_OUT (18): DSP Link Output to xHCI (Playback).
 *    - SOF_IPC4_GATEWAY_UAOL_IN (19): xHCI Link Input to DSP (Capture).
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_UAOL_GATEWAY_H__
#define __SOF_UAOL_GATEWAY_H__

#include <ntddk.h>
#include <wdf.h>
#include "Hw.h"
#include "Ipc.h"

/**
 * @def SOF_UAOL_TAG
 * @brief Pool allocation tag for UAOL memory allocations ('uaol').
 */
#define SOF_UAOL_TAG                'loau'

/* Intel IPC4 Gateway Node IDs for UAOL */
#define SOF_IPC4_GATEWAY_UAOL_OUT   18  /**< ipc4_alh_uaol_stream_link_output_class (DSP -> xHCI) */
#define SOF_IPC4_GATEWAY_UAOL_IN    19  /**< ipc4_alh_uaol_stream_link_input_class  (xHCI -> DSP) */

/* Link Stream Tags for USB Offload */
#define SOF_USB_PLAYBACK_STREAM_TAG 3   /**< Stream tag 3 in BAR0 Link DMA */
#define SOF_USB_CAPTURE_STREAM_TAG  4   /**< Stream tag 4 in BAR0 Link DMA */

/* Forward declaration */
struct _SOF_DEVICE_CONTEXT;

#pragma pack(push, 4)

/**
 * @struct _SOF_IPC4_UAOL_CONFIG_BLOB
 * @brief UAOL Stream Configuration BLOB passed in IPC4 Copier Gateway parameters.
 */
typedef struct _SOF_IPC4_UAOL_CONFIG_BLOB {
    UCHAR       XhciBus;            /**< PCI Bus of xHCI Controller (typically 0) */
    UCHAR       XhciDevice;         /**< PCI Device of xHCI Controller (typically 20 / 0x14) */
    UCHAR       XhciFunction;       /**< PCI Function of xHCI Controller (typically 0) */
    UCHAR       Reserved;
    USHORT      ArtDividerM;        /**< Audio Reference Time (ART) multiplier */
    USHORT      ArtDividerN;        /**< Audio Reference Time (ART) divider */
    ULONG       SampleRate;         /**< Sampling frequency in Hz */
    ULONG       Channels;           /**< Channel count (1..8) */
    ULONG       SampleBits;         /**< Container bit depth (16, 24, 32) */
    ULONG       ServiceIntervalUs;  /**< USB isochronous service interval in microseconds */
    ULONG       SioCreditSize;      /**< Hardware FIFO credit watermark in bytes */
    USHORT      FifoStartOffset;    /**< Offset within xHCI sideband SRAM FIFO */
    USHORT      ChannelMap;         /**< Channel slot mapping configuration */
} SOF_IPC4_UAOL_CONFIG_BLOB, *PSOF_IPC4_UAOL_CONFIG_BLOB;

/**
 * @struct _SOF_IPC4_UAOL_EP_ENTRY
 * @brief UAOL Endpoint Table Entry describing target USB device slot and endpoint address.
 */
typedef struct _SOF_IPC4_UAOL_EP_ENTRY {
    ULONG       UsbEpAddress     : 5; /**< USB endpoint address (1..15) */
    ULONG       DeviceSlotNumber : 8; /**< xHCI Device Slot assigned to UAC device */
    ULONG       SplitEp          : 1; /**< 1 if High-Speed split transaction required */
    ULONG       Reserved         : 18;
} SOF_IPC4_UAOL_EP_ENTRY, *PSOF_IPC4_UAOL_EP_ENTRY;

#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the UAOL subsystem and prepares Link DMA descriptors.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUaolInit(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx
);

/**
 * @brief Programs BAR0 Link DMA registers to stream audio frames to/from the xHCI sideband FIFO.
 *
 * @param[in] Ctx        Pointer to SOF device context.
 * @param[in] IsCapture  TRUE for USB microphone capture; FALSE for USB headphone/speaker playback.
 * @param[in] SampleRate Sampling rate in Hz.
 * @param[in] Channels   Channel count.
 * @param[in] BitDepth   Bit depth (16, 24, 32).
 * @param[in] BufferPa   Physical base address of circular audio buffer.
 * @param[in] BufferSize Total buffer size in bytes.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS
SofUaolProgramLinkDma(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ BOOLEAN IsCapture,
    _In_ ULONG SampleRate,
    _In_ ULONG Channels,
    _In_ ULONG BitDepth,
    _In_ PHYSICAL_ADDRESS BufferPa,
    _In_ ULONG BufferSize
);

/**
 * @brief Arms the Link DMA engine, starts the DSP IPC4 UAOL pipeline, and triggers sideband stream run.
 *
 * @param[in] Ctx       Pointer to SOF device context.
 * @param[in] IsCapture TRUE for capture; FALSE for playback.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUaolStartPipeline(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ BOOLEAN IsCapture
);

/**
 * @brief Suspends sideband streaming, halts Link DMA, and pauses the DSP IPC4 UAOL pipeline.
 *
 * @param[in] Ctx       Pointer to SOF device context.
 * @param[in] IsCapture TRUE for capture; FALSE for playback.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUaolStopPipeline(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ BOOLEAN IsCapture
);

/**
 * @brief Recovers from a hardware FIFO underflow or stall condition on the UAOL link.
 *
 * @param[in] Ctx       Pointer to SOF device context.
 * @param[in] IsCapture TRUE for capture; FALSE for playback.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUaolRecoverStall(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ BOOLEAN IsCapture
);

#ifdef __cplusplus
}
#endif

#endif /* __SOF_UAOL_GATEWAY_H__ */
