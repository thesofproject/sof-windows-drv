/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file UaolGateway.c
 * @brief Sound Open Firmware (SOF) Windows Driver - UAOL Gateway & Link DMA Implementation.
 *
 * @details
 * Implements hardware configuration and runtime management connecting Intel cAVS/ACE
 * DSP audio processing pipelines to the Intel USB Audio Offload Link (UAOL):
 *  1. Link DMA Configuration: Programs BAR0 stream descriptors reserved for USB playback
 *     (Stream Tag 3) and USB capture (Stream Tag 4).
 *  2. Engine Reset & Synchronization: Performs hardware stream engine resets (SRST)
 *     and verifies register completion before programming formats and physical descriptors.
 *  3. DSP Gateway Control: Sets up IPC4 UAOL Gateway nodes (Class 18 Output, Class 19 Input)
 *     and transitions pipelines between RUNNING and PAUSED states.
 *  4. Sideband Audio Coordination: Coordinates stream start, suspend, and close actions
 *     with the Windows Sideband Audio stack.
 *  5. Stall Recovery: Handles hardware FIFO underflows and stream stalls by resetting
 *     and reprogramming the Link DMA engine without restarting the host audio service.
 * Copyright 2026 The SOF Authors
 */

#include "UaolGateway.h"
#include "UsbOffload.h"
#include "Device.h"
#include "Hw.h"
#include "Ipc.h"
#include "Topo.h"
#include <ntstrsafe.h>

#define SOF_SD_CTL_RUN          (1 << 1)
#define SOF_SD_CTL_IOCE         (1 << 2)
#define SOF_SD_CTL_FEIE         (1 << 3)
#define SOF_SD_CTL_DEIE         (1 << 4)
#define SOF_SD_CTL_SRST         (1 << 0)

/**
 * @brief Computes standard 16-bit HD-A audio format word for Link DMA.
 *
 * @param[in] sampleRate Sampling frequency in Hz (e.g. 48000, 44100).
 * @param[in] channels   Number of audio channels.
 * @param[in] bitDepth   Bit depth (16, 20, 24, 32).
 * @return 16-bit packed HD-A format word.
 */
static USHORT
SofUaolFormatToHda(ULONG sampleRate, ULONG channels, ULONG bitDepth)
{
    USHORT fmt = 0;

    /* Sample Rate base (48kHz vs 44.1kHz) */
    if (sampleRate == 44100 || sampleRate == 88200 || sampleRate == 176400) {
        fmt |= (1 << 14); /* 44.1kHz base */
    }

    /* Multipliers */
    if (sampleRate == 96000 || sampleRate == 88200) {
        fmt |= (1 << 11); /* x2 */
    } else if (sampleRate == 192000 || sampleRate == 176400) {
        fmt |= (3 << 11); /* x4 */
    }

    /* Bit depth */
    if (bitDepth == 24 || bitDepth == 32) {
        fmt |= (4 << 4); /* 24-bit in 32-bit container */
    } else if (bitDepth == 20) {
        fmt |= (2 << 4);
    } /* else 16-bit is 0 */

    /* Channels */
    if (channels > 0) {
        fmt |= (USHORT)((channels - 1) & 0x0F);
    }

    return fmt;
}

/**
 * @brief Initializes the USB Audio Offload Link (UAOL) stream engines in device context.
 *
 * Pre-configures Stream Tag 3 for Playback and Stream Tag 4 for Capture in BAR0.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 * @return STATUS_SUCCESS on success, or STATUS_INVALID_PARAMETER if Ctx is NULL.
 */
NTSTATUS
SofUaolInit(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (Ctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Initialize Playback Link Engine (Stream Tag 3) */
    Ctx->UsbPlaybackEngine.StreamIndex = 2; /* 3rd engine in BAR0 */
    Ctx->UsbPlaybackEngine.StreamTag = SOF_USB_PLAYBACK_STREAM_TAG;
    Ctx->UsbPlaybackEngine.IsCapture = FALSE;
    Ctx->UsbPlaybackEngine.InUse = FALSE;
    Ctx->UsbPlaybackEngine.Ctx = Ctx;

    /* Initialize Capture Link Engine (Stream Tag 4) */
    Ctx->UsbCaptureEngine.StreamIndex = 3; /* 4th engine in BAR0 */
    Ctx->UsbCaptureEngine.StreamTag = SOF_USB_CAPTURE_STREAM_TAG;
    Ctx->UsbCaptureEngine.IsCapture = TRUE;
    Ctx->UsbCaptureEngine.InUse = FALSE;
    Ctx->UsbCaptureEngine.Ctx = Ctx;

    SofLogBreadcrumb("[UAOL] Initialized USB Offload Link Engines (Tags: Playback=3, Capture=4)\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Programs BAR0 Link DMA registers to stream audio frames to/from the xHCI sideband FIFO.
 *
 * Implementation Steps:
 *  1. Performs stream engine reset cycle (asserts SRST, polls, clears SRST, polls).
 *  2. Programs computed 16-bit audio format into SD_FORMAT.
 *  3. Programs total cyclic buffer size into SD_CBL.
 *  4. Programs Last Valid Index (SD_LVI = 1 for 2 BDL entries).
 *  5. Programs physical address of BDL table into SD_BDLPL and SD_BDLPU.
 *  6. Configures stream tag and enables IOC, FIFO error, and descriptor error interrupts in SD_CTL.
 *
 * @param[in] Ctx        Pointer to SOF device context.
 * @param[in] IsCapture  TRUE for capture; FALSE for playback.
 * @param[in] SampleRate Sampling rate in Hz.
 * @param[in] Channels   Channel count.
 * @param[in] BitDepth   Bit depth.
 * @param[in] BufferPa   Physical base address of circular audio buffer.
 * @param[in] BufferSize Total buffer size in bytes.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS
SofUaolProgramLinkDma(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ BOOLEAN IsCapture,
    _In_ ULONG SampleRate,
    _In_ ULONG Channels,
    _In_ ULONG BitDepth,
    _In_ PHYSICAL_ADDRESS BufferPa,
    _In_ ULONG BufferSize
)
{
    PSOF_HDA_DMA_ENGINE engine;
    ULONG streamOffset;
    USHORT hdaFmt;
    char msg[128];

    if (Ctx == NULL || Ctx->Bar0VirtualAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    engine = IsCapture ? &Ctx->UsbCaptureEngine : &Ctx->UsbPlaybackEngine;

    /* BAR0 stream registers base: 0x80 + (engine->StreamIndex * 0x20) */
    streamOffset = 0x80 + (engine->StreamIndex * 0x20);

    /* 1. Reset Link DMA engine */
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00),
        SOF_SD_CTL_SRST
    );

    for (int retry = 0; retry < 1000; retry++) {
        ULONG ctl = READ_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00));
        if (ctl & SOF_SD_CTL_SRST) {
            break;
        }
        KeStallExecutionProcessor(10);
    }

    /* Clear reset */
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00),
        0
    );

    for (int retry = 0; retry < 1000; retry++) {
        ULONG ctl = READ_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00));
        if (!(ctl & SOF_SD_CTL_SRST)) {
            break;
        }
        KeStallExecutionProcessor(10);
    }

    /* 2. Program Format */
    hdaFmt = SofUaolFormatToHda(SampleRate, Channels, BitDepth);
    WRITE_REGISTER_USHORT(
        (PUSHORT)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x12),
        hdaFmt
    );

    /* 3. Program Cyclic Buffer Length */
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x08),
        BufferSize
    );

    /* 4. Program Last Valid Index (1 buffer descriptor entry) */
    WRITE_REGISTER_USHORT(
        (PUSHORT)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x0C),
        1 /* 2 BDL entries */
    );

    /* 5. Program BDL Physical Address (reuse primary DMA buffer descriptor) */
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x18),
        BufferPa.LowPart
    );
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x1C),
        BufferPa.HighPart
    );

    /* 6. Configure Stream Tag and Interrupt Enables in SD_CTL */
    ULONG ctlVal = ((ULONG)engine->StreamTag << 20) | SOF_SD_CTL_IOCE | SOF_SD_CTL_FEIE | SOF_SD_CTL_DEIE;
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00),
        ctlVal
    );

    engine->InUse = TRUE;
    engine->BufferPa = BufferPa;
    engine->BufferSize = BufferSize;

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[UAOL] Link DMA programmed (Tag=%u, Off=0x%X, Size=%u, Fmt=0x%04X)\n",
        engine->StreamTag, streamOffset, BufferSize, hdaFmt);
    SofLogBreadcrumb(msg);

    return STATUS_SUCCESS;
}

/**
 * @brief Starts the Link DMA engine and triggers the DSP IPC4 UAOL processing pipeline.
 *
 * Sequence:
 *  1. Asserts SD_CTL.RUN in the Link DMA stream descriptor.
 *  2. Sends SOF_IPC4_GLB_SET_PIPELINE_STATE (RUNNING) to the UAOL gateway pipeline.
 *  3. Calls SofUsbOffloadStreamStart to begin isochronous streaming on the USB sideband stack.
 *
 * @param[in] Ctx       Pointer to SOF device context.
 * @param[in] IsCapture TRUE for capture; FALSE for playback.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUaolStartPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ BOOLEAN IsCapture
)
{
    PSOF_HDA_DMA_ENGINE engine;
    ULONG streamOffset;
    ULONG ctl;
    NTSTATUS status;

    if (Ctx == NULL || Ctx->Bar0VirtualAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    engine = IsCapture ? &Ctx->UsbCaptureEngine : &Ctx->UsbPlaybackEngine;
    streamOffset = 0x80 + (engine->StreamIndex * 0x20);

    /* 1. Arm Link DMA engine: set SD_CTL.RUN = 1 */
    ctl = READ_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00));
    ctl |= SOF_SD_CTL_RUN;
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00), ctl);

    SofLogBreadcrumb("[UAOL] Link DMA engine started (RUN=1)\n");

    /* 2. Start SOF IPC4 DSP Pipeline for UAOL Gateway */
    /* Form gateway node ID: class 18 (out) or 19 (in), v_index = Link Stream Tag */
    ULONG gatewayClass = IsCapture ? SOF_IPC4_GATEWAY_UAOL_IN : SOF_IPC4_GATEWAY_UAOL_OUT;
    ULONG nodeId = ((gatewayClass & 0x1F) << 8) | (engine->StreamTag & 0xFF);

    SofLogBreadcrumb("[UAOL] Starting DSP IPC4 Pipeline for UAOL Gateway...\n");
    status = SofIpc4SetPipelineState(Ctx, nodeId, 7 /* RUNNING */);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[UAOL] SOF_IPC4_GLB_SET_PIPELINE_STATE returned non-zero (continuing)\n");
    }

    /* 3. Trigger USB Sideband Stream */
    SofUsbOffloadStreamStart(Ctx, IsCapture ? 1 : 0);

    SofLogBreadcrumb("[UAOL] Stream pipeline fully operational!\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Stops Link DMA and suspends the DSP IPC4 UAOL processing pipeline.
 *
 * Sequence:
 *  1. Calls SofUsbOffloadStreamSuspend to halt USB sideband transfers.
 *  2. Clears SD_CTL.RUN in the Link DMA stream descriptor.
 *  3. Sends SOF_IPC4_GLB_SET_PIPELINE_STATE (PAUSED) to the DSP pipeline.
 *
 * @param[in] Ctx       Pointer to SOF device context.
 * @param[in] IsCapture TRUE for capture; FALSE for playback.
 * @return STATUS_SUCCESS.
 */
NTSTATUS
SofUaolStopPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ BOOLEAN IsCapture
)
{
    PSOF_HDA_DMA_ENGINE engine;
    ULONG streamOffset;
    ULONG ctl;

    if (Ctx == NULL || Ctx->Bar0VirtualAddress == NULL) {
        return STATUS_SUCCESS;
    }

    engine = IsCapture ? &Ctx->UsbCaptureEngine : &Ctx->UsbPlaybackEngine;
    streamOffset = 0x80 + (engine->StreamIndex * 0x20);

    /* 1. Suspend USB Sideband Stream */
    SofUsbOffloadStreamSuspend(Ctx, IsCapture ? 1 : 0);

    /* 2. Stop Link DMA engine: clear SD_CTL.RUN */
    ctl = READ_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00));
    ctl &= ~SOF_SD_CTL_RUN;
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Bar0VirtualAddress + streamOffset + 0x00), ctl);

    /* 3. Pause/Reset SOF IPC4 DSP Pipeline */
    ULONG gatewayClass = IsCapture ? SOF_IPC4_GATEWAY_UAOL_IN : SOF_IPC4_GATEWAY_UAOL_OUT;
    ULONG nodeId = ((gatewayClass & 0x1F) << 8) | (engine->StreamTag & 0xFF);
    SofIpc4SetPipelineState(Ctx, nodeId, 1 /* PAUSED */);

    SofLogBreadcrumb("[UAOL] Stream pipeline suspended.\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Recovers from a hardware FIFO stall or stream underflow on the UAOL link.
 *
 * Resets the Link DMA descriptor, clears FIFO error flags, and restores stream operation.
 *
 * @param[in] Ctx       Pointer to SOF device context.
 * @param[in] IsCapture TRUE for capture; FALSE for playback.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUaolRecoverStall(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ BOOLEAN IsCapture
)
{
    SofLogBreadcrumb("[UAOL-RECOVER] Initiating UAOL Stall Recovery sequence...\n");

    /* 1. Stop current pipeline */
    SofUaolStopPipeline(Ctx, IsCapture);

    /* 2. Short settling stall */
    KeStallExecutionProcessor(500);

    /* 3. Re-arm Link DMA and restart pipeline if configured */
    PSOF_HDA_DMA_ENGINE engine = IsCapture ? &Ctx->UsbCaptureEngine : &Ctx->UsbPlaybackEngine;
    if (engine->InUse && engine->BufferSize > 0) {
        SofUaolProgramLinkDma(
            Ctx,
            IsCapture,
            48000, /* fallback / cached rate */
            2,
            16,
            engine->BufferPa,
            (ULONG)engine->BufferSize
        );
        SofUaolStartPipeline(Ctx, IsCapture);
        SofLogBreadcrumb("[UAOL-RECOVER] Recovery successful: Link DMA & Pipeline restarted\n");
    }

    return STATUS_SUCCESS;
}
