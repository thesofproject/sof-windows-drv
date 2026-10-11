/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file WaveRt.c
 * @brief PortCls WaveRT Audio Streaming Engine Implementation for Sound Open Firmware.
 *
 * @details
 * Implements the core streaming engine for Windows audio endpoints:
 *  1. Circular Ring Buffer & BDL Setup: Allocates non-paged contiguous physical memory
 *     for audio sample exchange between user-mode (audiosrv) and kernel/hardware. Builds
 *     a 2-entry double-buffered Buffer Descriptor List (BDL) with Interrupt-On-Completion (IOC)
 *     set on both halves to guarantee smooth circular wrapping.
 *  2. Timer & Periodic Event Notifications: Operates a high-resolution kernel timer and
 *     Deferred Procedure Call (DPC) to signal registered KEVENTs at precise period intervals
 *     (typically 10ms = 100Hz), enabling glitch-free audio streaming.
 *  3. BAR0 HD-A Stream Descriptor Programming: Configures hardware stream engines (SD_CTL,
 *     SD_CBL, SD_FORMAT, SD_BDLPL/PU, SD_LVI, SD_STS) and decouples host DMA directly into
 *     the DSP FIFO using Processing Pipe Control (PPCTL).
 *  4. Processing Pipe Link Control (PPLC) & Multi-Link (ML): Programs link DMA engines for
 *     routing DSP audio across external physical interfaces (HD-A codec link, Display Audio /
 *     HDMI multi-link pipes).
 *  5. Dynamic Audio Graph Assembly: Instantiates and binds DSP IPC4 pipelines (Host Copier ->
 *     Gain -> DAI Copier) on demand across diverse endpoints (Analog HD-A, HDMI 1-3, DMIC,
 *     SSP2, SoundWire, and USB Offload).
 *  6. Volume & Mute Controls: Manages volume levels in 1/65536 dB units (clamped to [-96dB, 0dB])
 *     and dispatches IPC4 messages to hardware Gain modules in DSP SRAM.
 * Copyright 2026 The SOF Authors
 */

#include "WaveRt.h"
#include "Device.h"
#include "HdaVerb.h"

#ifdef _KERNEL_MODE
static KDEFERRED_ROUTINE SofWaveRtTimerDpc;

/**
 * @brief Periodic timer DPC callback routine.
 *
 * Invoked on high-resolution timer expiration. Advances the linear position byte counter
 * and signals all registered WaveRT client notification events.
 *
 * @param[in]     Dpc             Pointer to DPC object.
 * @param[in,opt] DeferredContext Context pointer (SOF_WAVERT_STREAM).
 * @param[in,opt] SystemArgument1 Unused system argument.
 * @param[in,opt] SystemArgument2 Unused system argument.
 */
static VOID SofWaveRtTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PSOF_WAVERT_STREAM stream = (PSOF_WAVERT_STREAM)DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (stream != NULL && stream->State == KSSTATE_RUN) {
        if (stream->BufferSizeBytes > 0 && stream->PeriodSizeBytes > 0) {
            stream->LinearPositionBytes += stream->PeriodSizeBytes;
        }
        SofWaveRtSignalNotification(stream);
    }
}
#endif

/**
 * @brief Allocates and initializes a new WaveRT audio stream.
 *
 * Performs:
 *  1. Stream descriptor allocation and endpoint-to-pipeline mapping.
 *  2. Cyclic audio buffer and 2-entry BDL physical memory allocation.
 *  3. On-demand DSP IPC4 audio pipeline creation (Host Copier -> Gain -> DAI Copier).
 *  4. BAR0 HD-A stream engine programming and decoupling via PPCTL.
 *  5. Link DMA (PPLC) configuration for codec and display audio links.
 *
 * @param[in]  Ctx             Pointer to SOF device context.
 * @param[in]  EndpointType    Target endpoint type (e.g. HDA_ANALOG, HDMI_1, DMIC_PDM).
 * @param[in]  IsCapture       TRUE for recording; FALSE for playback.
 * @param[in]  StreamTag       Preferred HD-A stream tag (1..15); 0 for default.
 * @param[in]  Format          Audio format specification.
 * @param[in]  BufferSizeBytes Requested circular buffer size in bytes.
 * @param[in]  PeriodMs        Period duration in milliseconds.
 * @param[out] StreamOut       Receives allocated SOF_WAVERT_STREAM pointer.
 * @return STATUS_SUCCESS on success, or an NTSTATUS failure code.
 */
NTSTATUS SofWaveRtCreateStream(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ SOF_ENDPOINT_TYPE EndpointType,
    _In_ BOOLEAN IsCapture,
    _In_ ULONG StreamTag,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Format,
    _In_ ULONG BufferSizeBytes,
    _In_ ULONG PeriodMs,
    _Out_ PSOF_WAVERT_STREAM *StreamOut
)
{
    PSOF_WAVERT_STREAM stream;
    NTSTATUS status;
    ULONG bdlSize;

    if (StreamOut == NULL || Format == NULL || BufferSizeBytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }

#ifdef _KERNEL_MODE
    stream = (PSOF_WAVERT_STREAM)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SOF_WAVERT_STREAM), SOF_DEVICE_TAG);
#else
    stream = (PSOF_WAVERT_STREAM)calloc(1, sizeof(SOF_WAVERT_STREAM));
#endif
    if (stream == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(stream, sizeof(SOF_WAVERT_STREAM));
    stream->DeviceContext = Ctx;
    stream->EndpointType = EndpointType;
    stream->IsCapture = IsCapture;

    /* Assign pipeline IDs, stream tags, and hardware stream descriptor indices */
    if (IsCapture) {
        if (EndpointType == SOF_ENDPOINT_DMIC_PDM) {
            stream->PipelineId = SOF_PIPELINE_ID_DMIC;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_DMIC;
            stream->StreamIndex = 1;
        } else if (EndpointType == SOF_ENDPOINT_I2S_SSP2) {
            stream->PipelineId = SOF_PIPELINE_ID_SSP_CAPTURE;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SSP_CAPTURE;
            stream->StreamIndex = 2;
        } else if (EndpointType == SOF_ENDPOINT_SDW_CAPTURE) {
            stream->PipelineId = SOF_PIPELINE_ID_SDW_CAPTURE;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SDW_CAPTURE;
            stream->StreamIndex = 3;
        } else {
            stream->PipelineId = SOF_PIPELINE_ID_CAPTURE;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_CAPTURE;
            stream->StreamIndex = 0;
        }
    } else if (EndpointType == SOF_ENDPOINT_HDMI_1) {
        stream->PipelineId = SOF_PIPELINE_ID_HDMI_1;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_HDMI_1;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 1 : 8;
    } else if (EndpointType == SOF_ENDPOINT_HDMI_2) {
        stream->PipelineId = SOF_PIPELINE_ID_HDMI_2;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_HDMI_2;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 2 : 9;
    } else if (EndpointType == SOF_ENDPOINT_HDMI_3) {
        stream->PipelineId = SOF_PIPELINE_ID_HDMI_3;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_HDMI_3;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 3 : 10;
    } else if (EndpointType == SOF_ENDPOINT_I2S_SSP2) {
        stream->PipelineId = SOF_PIPELINE_ID_SSP_PLAYBACK;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SSP_RENDER;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 4 : 11;
    } else if (EndpointType == SOF_ENDPOINT_SDW_RENDER) {
        stream->PipelineId = SOF_PIPELINE_ID_SDW_PLAYBACK;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SDW_RENDER;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 5 : 12;
    } else {
        stream->PipelineId = SOF_PIPELINE_ID_PLAYBACK;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_PLAYBACK;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams : 7;
    }

    stream->Format = *Format;
    stream->BufferSizeBytes = BufferSizeBytes;
    stream->PeriodMs = (PeriodMs > 0) ? PeriodMs : SOF_WAVERT_DEFAULT_PERIOD_MS;
    stream->State = KSSTATE_STOP;
    stream->HostCopierInstance = 0xFFFFFFFF;
    stream->GainInstance = 0xFFFFFFFF;
    stream->DaiCopierInstance = 0xFFFFFFFF;

    /* Calculate period size in bytes: (Rate * Ch * BytesPerSample * PeriodMs) / 1000 */
    stream->PeriodSizeBytes = (Format->SamplingFrequency *
                               Format->ChannelsCount *
                               (Format->BitDepth / 8) *
                               stream->PeriodMs) / 1000;

    /* 1. Allocate circular ring buffer */
#ifdef _KERNEL_MODE
    {
        PHYSICAL_ADDRESS maxAddr;
        maxAddr.QuadPart = 0xFFFFFFFFFFFFFFFFULL;

        stream->KernelBuffer = MmAllocateContiguousMemory(stream->BufferSizeBytes, maxAddr);
        if (stream->KernelBuffer == NULL) {
            maxAddr.QuadPart = 0xFFFFFFFFULL;
            stream->KernelBuffer = MmAllocateContiguousMemory(stream->BufferSizeBytes, maxAddr);
        }
        if (stream->KernelBuffer == NULL) {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[WaveRt] MmAllocateContiguousMemory(%u) FAILED\n", stream->BufferSizeBytes);
            SofLogBreadcrumb(bmsg);
            ExFreePoolWithTag(stream, SOF_DEVICE_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(stream->KernelBuffer, stream->BufferSizeBytes);
        stream->PhysicalAddress = MmGetPhysicalAddress(stream->KernelBuffer);

        /* 2. Allocate 4KB BDL */
        bdlSize = 4096;
        maxAddr.QuadPart = 0xFFFFFFFFFFFFFFFFULL;
        stream->Bdl = (PSOF_INTEL_DSP_BDL)MmAllocateContiguousMemory(bdlSize, maxAddr);
        if (stream->Bdl == NULL) {
            maxAddr.QuadPart = 0xFFFFFFFFULL;
            stream->Bdl = (PSOF_INTEL_DSP_BDL)MmAllocateContiguousMemory(bdlSize, maxAddr);
        }
        if (stream->Bdl == NULL) {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[WaveRt] MmAllocateContiguousMemory BDL FAILED\n");
            SofLogBreadcrumb(bmsg);
            MmFreeContiguousMemory(stream->KernelBuffer);
            ExFreePoolWithTag(stream, SOF_DEVICE_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(stream->Bdl, bdlSize);
        stream->BdlPhysicalAddress = MmGetPhysicalAddress(stream->Bdl);

        {
            char bmsg[192];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] Buffer=%p (Phys=0x%llx, %u B), BDL=%p (Phys=0x%llx), StreamIndex=%u, Tag=%u\n",
                stream->KernelBuffer, stream->PhysicalAddress.QuadPart, stream->BufferSizeBytes,
                stream->Bdl, stream->BdlPhysicalAddress.QuadPart,
                stream->StreamIndex, stream->StreamTag);
            SofLogBreadcrumb(bmsg);
        }

        /*
         * Populate 2 BDL entries for double buffering / circular period wrap:
         * Entry 0: First half of ring buffer, IOC asserted.
         * Entry 1: Second half of ring buffer, IOC asserted.
         */
        stream->Bdl[0].AddrLow = stream->PhysicalAddress.LowPart;
        stream->Bdl[0].AddrHigh = stream->PhysicalAddress.HighPart;
        stream->Bdl[0].Size = stream->BufferSizeBytes / 2;
        stream->Bdl[0].Ioc = 1;

        stream->Bdl[1].AddrLow = (ULONG)(stream->PhysicalAddress.LowPart + (stream->BufferSizeBytes / 2));
        stream->Bdl[1].AddrHigh = stream->PhysicalAddress.HighPart;
        stream->Bdl[1].Size = stream->BufferSizeBytes / 2;
        stream->Bdl[1].Ioc = 1;

        /* Initialize timer and DPC for period notifications */
        KeInitializeDpc(&stream->NotificationDpc, SofWaveRtTimerDpc, stream);
        KeInitializeTimer(&stream->NotificationTimer);
    }
#else
    stream->KernelBuffer = calloc(1, stream->BufferSizeBytes);
    bdlSize = sizeof(SOF_INTEL_DSP_BDL) * 2;
    stream->Bdl = (PSOF_INTEL_DSP_BDL)calloc(1, bdlSize);
    if (stream->KernelBuffer == NULL || stream->Bdl == NULL) {
        if (stream->KernelBuffer) free(stream->KernelBuffer);
        if (stream->Bdl) free(stream->Bdl);
        free(stream);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    stream->PhysicalAddress.QuadPart = 0x10000000ULL;
    stream->BdlPhysicalAddress.QuadPart = 0x20000000ULL;
    stream->Bdl[0].AddrLow = stream->PhysicalAddress.LowPart;
    stream->Bdl[0].Size = stream->BufferSizeBytes / 2;
    stream->Bdl[0].Ioc = 1;
    stream->Bdl[1].AddrLow = stream->PhysicalAddress.LowPart + (stream->BufferSizeBytes / 2);
    stream->Bdl[1].Size = stream->BufferSizeBytes / 2;
    stream->Bdl[1].Ioc = 1;
#endif

    /* Initialize stream volume and mute from device context defaults */
    if (Ctx != NULL && EndpointType < SOF_ENDPOINT_COUNT) {
        stream->VolumeLevelDb[0] = Ctx->DefaultVolumeLevelDb[EndpointType][0];
        stream->VolumeLevelDb[1] = Ctx->DefaultVolumeLevelDb[EndpointType][1];
        stream->MasterMute = Ctx->DefaultMute[EndpointType];
        stream->IsMuted[0] = stream->MasterMute;
        stream->IsMuted[1] = stream->MasterMute;
    }

    /* 3. Create SOF IPC4 audio pipeline on-demand based on endpoint */
    switch (EndpointType) {
    case SOF_ENDPOINT_HDMI_1:
        status = SofIpc4CreateHdmiPipeline(Ctx, stream->PipelineId, stream->StreamTag, 1, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        break;
    case SOF_ENDPOINT_HDMI_2:
        status = SofIpc4CreateHdmiPipeline(Ctx, stream->PipelineId, stream->StreamTag, 2, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        break;
    case SOF_ENDPOINT_HDMI_3:
        status = SofIpc4CreateHdmiPipeline(Ctx, stream->PipelineId, stream->StreamTag, 3, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        break;
    case SOF_ENDPOINT_DMIC_PDM:
        status = SofIpc4CreateDmicPipeline(Ctx, stream->PipelineId, stream->StreamTag, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        if (!NT_SUCCESS(status)) {
            char bmsg[160];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] DMIC hardware not connected (0x%08x), operating in silence capture mode\n", status);
            SofLogBreadcrumb(bmsg);
            stream->HostCopierInstance = 0xFFFFFFFF;
            stream->GainInstance = 0xFFFFFFFF;
            stream->DaiCopierInstance = 0xFFFFFFFF;
            status = STATUS_SUCCESS;
        }
        break;
    case SOF_ENDPOINT_I2S_SSP2:
        status = SofIpc4CreateSspPipeline(Ctx, stream->PipelineId, stream->StreamTag, 2, IsCapture, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        if (!NT_SUCCESS(status)) {
            char bmsg[160];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] SSP2 hardware not connected (0x%08x), operating in zero data mode\n", status);
            SofLogBreadcrumb(bmsg);
            stream->HostCopierInstance = 0xFFFFFFFF;
            stream->GainInstance = 0xFFFFFFFF;
            stream->DaiCopierInstance = 0xFFFFFFFF;
            status = STATUS_SUCCESS;
        }
        break;
    case SOF_ENDPOINT_SDW_RENDER:
    case SOF_ENDPOINT_SDW_CAPTURE:
        status = SofIpc4CreateAlhPipeline(Ctx, stream->PipelineId, stream->StreamTag, 0 /* Link 0 */, IsCapture, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        if (!NT_SUCCESS(status)) {
            char bmsg[160];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] SoundWire hardware not connected (0x%08x), operating in %s mode\n",
                status, IsCapture ? "silence capture" : "zero data playback");
            SofLogBreadcrumb(bmsg);
            stream->HostCopierInstance = 0xFFFFFFFF;
            stream->GainInstance = 0xFFFFFFFF;
            stream->DaiCopierInstance = 0xFFFFFFFF;
            status = STATUS_SUCCESS;
        }
        break;
    case SOF_ENDPOINT_HDA_ANALOG:
    default:
        if (IsCapture) {
            status = SofIpc4CreateCapturePipeline(Ctx, stream->PipelineId, stream->StreamTag, Format,
                                                  &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        } else {
            status = SofIpc4CreatePlaybackPipeline(Ctx, stream->PipelineId, stream->StreamTag, Format,
                                                   &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        }
        break;
    }

    if (!NT_SUCCESS(status)) {
        SofWaveRtFreeStream(stream);
        return status;
    }

    /* Apply cached volume and mute to newly instantiated Gain module */
    if (stream->GainInstance != 0xFFFFFFFF) {
        if (stream->MasterMute) {
            SofIpc4SetMute(Ctx, stream->GainInstance, SOF_IPC4_GAIN_ALL_CHANNELS_MASK, TRUE, 0);
        } else if (stream->VolumeLevelDb[0] != 0 || stream->VolumeLevelDb[1] != 0) {
            if (stream->VolumeLevelDb[0] == stream->VolumeLevelDb[1]) {
                SofIpc4SetVolumeLevel(Ctx, stream->GainInstance, SOF_IPC4_GAIN_ALL_CHANNELS_MASK, stream->VolumeLevelDb[0]);
            } else {
                SofIpc4SetVolumeLevel(Ctx, stream->GainInstance, 0, stream->VolumeLevelDb[0]);
                SofIpc4SetVolumeLevel(Ctx, stream->GainInstance, 1, stream->VolumeLevelDb[1]);
            }
        }
    }

    /* 4. Configure BAR0 HDA Stream Engine (if DSP pipeline instantiated) */
    if (stream->HostCopierInstance != 0xFFFFFFFF) {
        SofWaveRtConfigureHdaStream(stream);

        /* 5. Configure Link Stream Engine (PPLC) */
        SofWaveRtConfigureLinkStream(stream);
    }

    stream->IsActive = TRUE;
    stream->State = KSSTATE_ACQUIRE;
    *StreamOut = stream;
    return STATUS_SUCCESS;
}

/**
 * @brief Teardown and deallocation of a WaveRT stream instance.
 *
 * Transitions stream to KSSTATE_STOP, caches active volume/mute state in device context,
 * unbinds and deletes all DSP modules and pipeline objects, cancels timers, and frees memory.
 *
 * @param[in] Stream Pointer to stream instance to free.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtFreeStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
    if (Stream == NULL) {
        return STATUS_SUCCESS;
    }

    /* Stop stream and reset pipeline */
    SofWaveRtSetState(Stream, KSSTATE_STOP);

    /* Cache stream volume and mute back into device context */
    if (Stream->DeviceContext != NULL && Stream->EndpointType < SOF_ENDPOINT_COUNT) {
        Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][0] = Stream->VolumeLevelDb[0];
        Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][1] = Stream->VolumeLevelDb[1];
        Stream->DeviceContext->DefaultMute[Stream->EndpointType] = Stream->MasterMute;
    }

    /* Unbind and delete SOF IPC4 Pipeline on-demand */
    if (Stream->DeviceContext != NULL) {
        if (Stream->HostCopierInstance != 0xFFFFFFFF && Stream->GainInstance != 0xFFFFFFFF) {
            if (Stream->IsCapture) {
                SofIpc4UnbindModules(Stream->DeviceContext,
                                     SOF_IPC4_MOD_ID_GAIN, Stream->GainInstance, 0,
                                     SOF_IPC4_MOD_ID_COPIER, Stream->HostCopierInstance, 0);
            } else {
                SofIpc4UnbindModules(Stream->DeviceContext,
                                     SOF_IPC4_MOD_ID_COPIER, Stream->HostCopierInstance, 0,
                                     SOF_IPC4_MOD_ID_GAIN, Stream->GainInstance, 0);
            }
        }
        if (Stream->GainInstance != 0xFFFFFFFF && Stream->DaiCopierInstance != 0xFFFFFFFF) {
            if (Stream->IsCapture) {
                SofIpc4UnbindModules(Stream->DeviceContext,
                                     SOF_IPC4_MOD_ID_COPIER, Stream->DaiCopierInstance, 0,
                                     SOF_IPC4_MOD_ID_GAIN, Stream->GainInstance, 0);
            } else {
                SofIpc4UnbindModules(Stream->DeviceContext,
                                     SOF_IPC4_MOD_ID_GAIN, Stream->GainInstance, 0,
                                     SOF_IPC4_MOD_ID_COPIER, Stream->DaiCopierInstance, 0);
            }
        }

        if (Stream->HostCopierInstance != 0xFFFFFFFF) {
            SofIpc4DeletePipeline(Stream->DeviceContext, Stream->PipelineId);
            SofIpc4FreeInstanceId(Stream->DeviceContext, SOF_IPC4_MOD_ID_COPIER, Stream->HostCopierInstance);
            Stream->HostCopierInstance = 0xFFFFFFFF;
        }
        if (Stream->GainInstance != 0xFFFFFFFF) {
            SofIpc4FreeInstanceId(Stream->DeviceContext, SOF_IPC4_MOD_ID_GAIN, Stream->GainInstance);
            Stream->GainInstance = 0xFFFFFFFF;
        }
        if (Stream->DaiCopierInstance != 0xFFFFFFFF) {
            SofIpc4FreeInstanceId(Stream->DeviceContext, SOF_IPC4_MOD_ID_COPIER, Stream->DaiCopierInstance);
            Stream->DaiCopierInstance = 0xFFFFFFFF;
        }

#ifdef _KERNEL_MODE
        KeStallExecutionProcessor(500);
#endif
    }

#ifdef _KERNEL_MODE
    if (Stream->NotificationEvents[0] != NULL) {
        KeCancelTimer(&Stream->NotificationTimer);
    }

    if (Stream->Bdl != NULL) {
        MmFreeContiguousMemory(Stream->Bdl);
        Stream->Bdl = NULL;
    }

    if (Stream->KernelBuffer != NULL) {
        MmFreeContiguousMemory(Stream->KernelBuffer);
        Stream->KernelBuffer = NULL;
    }

    ExFreePoolWithTag(Stream, SOF_DEVICE_TAG);
#else
    if (Stream->Bdl != NULL) {
        free(Stream->Bdl);
    }
    if (Stream->KernelBuffer != NULL) {
        free(Stream->KernelBuffer);
    }
    free(Stream);
#endif

    return STATUS_SUCCESS;
}

/**
 * @brief Controls stream state transitions (RUN, PAUSE, STOP, ACQUIRE).
 *
 * Sequence for RUN:
 *  1. Checks and initializes Display Audio Codec (CAD 2) if detected.
 *  2. Starts codec converters (DAC or ADC) via immediate verbs.
 *  3. Starts Link Stream DMA (PPLC) and Host HD-A Stream DMA.
 *  4. Sends SOF_IPC4_PIPE_RUNNING to DSP firmware.
 *  5. Arms high-resolution periodic notification timer.
 *
 * Sequence for PAUSE / STOP:
 *  1. Cancels notification timer.
 *  2. Sends SOF_IPC4_PIPE_PAUSED or SOF_IPC4_PIPE_RESET to DSP firmware.
 *  3. Halts Host DMA and Link DMA engines.
 *  4. Stops codec converters.
 *
 * @param[in] Stream   Pointer to stream instance.
 * @param[in] NewState Target KSSTATE.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofWaveRtSetState(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ KSSTATE NewState
)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Stream->State == NewState) {
        return STATUS_SUCCESS;
    }

    switch (NewState) {
    case KSSTATE_RUN:
        if (Stream->DeviceContext != NULL && !Stream->DeviceContext->IDispCodecPresent && Stream->DeviceContext->Bar0VirtualAddress) {
            USHORT wakests = readw((PUCHAR)Stream->DeviceContext->Bar0VirtualAddress + SOF_HDA_WAKESTS);
            if (wakests & SOF_HDA_WAKESTS_SDIN2) {
                Stream->DeviceContext->IDispCodecPresent = TRUE;
                if (NT_SUCCESS(SofHdaProbeDisplayCodec(Stream->DeviceContext, 2, &Stream->DeviceContext->GfxCodecVendorId, &Stream->DeviceContext->GfxCodecRevisionId))) {
                    SofHdaInitDisplayCodec(Stream->DeviceContext, 2);
                }
            }
        }

        /* Start Codec Converter (DAC or ADC) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->HdaCodecPresent && Stream->EndpointType == SOF_ENDPOINT_HDA_ANALOG) {
            SofHdaStartConverter(Stream->DeviceContext, 0, Stream->IsCapture, Stream->StreamTag);
        } else if (Stream->DeviceContext != NULL && Stream->DeviceContext->IDispCodecPresent && Stream->EndpointType >= SOF_ENDPOINT_HDMI_1 && Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG hdmiLink = 1 + (Stream->EndpointType - SOF_ENDPOINT_HDMI_1);
            SofHdaStartDisplayConverter(Stream->DeviceContext, 2, hdmiLink, Stream->StreamTag, &Stream->Format);
        }

        /* Start Link Stream DMA (PPLC) and HDA Stream Engine */
        if (Stream->HostCopierInstance != 0xFFFFFFFF) {
            SofWaveRtStartLinkStream(Stream);
            SofWaveRtStartHdaStream(Stream);

            /* Set DSP Pipeline state to RUNNING */
            status = SofIpc4SetPipelineState(Stream->DeviceContext, Stream->PipelineId, SOF_IPC4_PIPE_RUNNING);
            {
                char bmsg[128];
                RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[WaveRt] SetState RUN: status=0x%08x (idx=%u, pipe=%u)\n",
                    status, Stream->StreamIndex, Stream->PipelineId);
                SofLogBreadcrumb(bmsg);
            }
        } else {
            /* Zero out buffer on run for silence / 0s */
            if (Stream->KernelBuffer != NULL && Stream->BufferSizeBytes > 0) {
                RtlZeroMemory(Stream->KernelBuffer, Stream->BufferSizeBytes);
            }
        }

#ifdef _KERNEL_MODE
        /* Start periodic notification timer */
        {
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -10000LL * Stream->PeriodMs; /* Relative 100ns intervals */
            KeSetTimerEx(&Stream->NotificationTimer, dueTime, Stream->PeriodMs, &Stream->NotificationDpc);
        }
#endif
        Stream->State = KSSTATE_RUN;
        break;

    case KSSTATE_PAUSE:
#ifdef _KERNEL_MODE
        KeCancelTimer(&Stream->NotificationTimer);
#endif
        if (Stream->HostCopierInstance != 0xFFFFFFFF) {
            /* Set DSP Pipeline state to PAUSED */
            status = SofIpc4SetPipelineState(Stream->DeviceContext, Stream->PipelineId, SOF_IPC4_PIPE_PAUSED);

            /* Stop HDA Stream Engine (Host DMA) */
            SofWaveRtStopHdaStream(Stream);

            /* Stop Link Stream DMA (PPLC) */
            SofWaveRtStopLinkStream(Stream);
        }

        /* Stop Codec Converter (DAC or ADC) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->HdaCodecPresent && Stream->EndpointType == SOF_ENDPOINT_HDA_ANALOG) {
            SofHdaStopConverter(Stream->DeviceContext, 0, Stream->IsCapture);
        } else if (Stream->DeviceContext != NULL && Stream->DeviceContext->IDispCodecPresent && Stream->EndpointType >= SOF_ENDPOINT_HDMI_1 && Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG hdmiLink = 1 + (Stream->EndpointType - SOF_ENDPOINT_HDMI_1);
            SofHdaStopDisplayConverter(Stream->DeviceContext, 2, hdmiLink);
        }
        Stream->State = KSSTATE_PAUSE;
        break;

    case KSSTATE_STOP:
    case KSSTATE_ACQUIRE:
#ifdef _KERNEL_MODE
        KeCancelTimer(&Stream->NotificationTimer);
#endif
        Stream->LinearPositionBytes = 0;
        if (Stream->HostCopierInstance != 0xFFFFFFFF) {
            /* Reset DSP Pipeline */
            status = SofIpc4SetPipelineState(Stream->DeviceContext, Stream->PipelineId, SOF_IPC4_PIPE_RESET);

            /* Reset HDA Stream Engine */
            SofWaveRtResetHdaStream(Stream);

            /* Reset Link Stream DMA (PPLC) */
            SofWaveRtResetLinkStream(Stream);
        }

        /* Stop Codec Converter (DAC or ADC) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->HdaCodecPresent && Stream->EndpointType == SOF_ENDPOINT_HDA_ANALOG) {
            SofHdaStopConverter(Stream->DeviceContext, 0, Stream->IsCapture);
        } else if (Stream->DeviceContext != NULL && Stream->DeviceContext->IDispCodecPresent && Stream->EndpointType >= SOF_ENDPOINT_HDMI_1 && Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG hdmiLink = 1 + (Stream->EndpointType - SOF_ENDPOINT_HDMI_1);
            SofHdaStopDisplayConverter(Stream->DeviceContext, 2, hdmiLink);
        }
        Stream->State = NewState;
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    return status;
}

/**
 * @brief Returns current stream byte position within the circular buffer.
 *
 * @param[in]  Stream        Pointer to stream instance.
 * @param[out] PositionBytes Receives byte offset (0..BufferSizeBytes-1).
 * @return STATUS_SUCCESS on success; STATUS_INVALID_PARAMETER if invalid args.
 */
NTSTATUS SofWaveRtGetPosition(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PULONG PositionBytes
)
{
    if (Stream == NULL || PositionBytes == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Stream->BufferSizeBytes > 0) {
        *PositionBytes = Stream->LinearPositionBytes % Stream->BufferSizeBytes;
    } else {
        *PositionBytes = 0;
    }
    return STATUS_SUCCESS;
}

/**
 * @brief Reads the hardware 24 MHz HD-A Wall Clock counter register (WALCLK).
 *
 * Converts 24 MHz ticks to elapsed nanoseconds:
 *   Nanoseconds = (Ticks * 1,000,000,000) / 24,000,000.
 *
 * @param[in]  Stream           Pointer to stream instance.
 * @param[out] ClockNanoseconds Receives wall clock time in nanoseconds.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtGetClockTime(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PULONGLONG ClockNanoseconds
)
{
    if (Stream == NULL || ClockNanoseconds == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

#ifdef _KERNEL_MODE
    if (Stream->DeviceContext != NULL && Stream->DeviceContext->Bar0VirtualAddress != NULL) {
        ULONG ticks = readl((PUCHAR)Stream->DeviceContext->Bar0VirtualAddress + SOF_HDA_REG_WALCLK);
        /* Intel HDA Wall Clock runs at 24.0 MHz */
        *ClockNanoseconds = ((ULONGLONG)ticks * 1000000000ULL) / 24000000ULL;
        return STATUS_SUCCESS;
    }
#endif

    *ClockNanoseconds = 0;
    return STATUS_SUCCESS;
}

/**
 * @brief Registers a client notification event to be signaled on period boundaries.
 *
 * @param[in] Stream            Pointer to stream instance.
 * @param[in] NotificationEvent Pointer to KEVENT object.
 * @return STATUS_SUCCESS on success; STATUS_INSUFFICIENT_RESOURCES if table full.
 */
NTSTATUS SofWaveRtRegisterNotificationEvent(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ PVOID NotificationEvent
)
{
    if (Stream == NULL || NotificationEvent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Stream->NumNotificationEvents >= SOF_WAVERT_MAX_EVENTS) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Stream->NotificationEvents[Stream->NumNotificationEvents++] = NotificationEvent;
    return STATUS_SUCCESS;
}

/**
 * @brief Unregisters a client notification event.
 *
 * @param[in] Stream            Pointer to stream instance.
 * @param[in] NotificationEvent Pointer to KEVENT object to remove.
 * @return STATUS_SUCCESS on success; STATUS_NOT_FOUND if event was not registered.
 */
NTSTATUS SofWaveRtUnregisterNotificationEvent(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ PVOID NotificationEvent
)
{
    ULONG i, j;

    if (Stream == NULL || NotificationEvent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < Stream->NumNotificationEvents; i++) {
        if (Stream->NotificationEvents[i] == NotificationEvent) {
            for (j = i; j < Stream->NumNotificationEvents - 1; j++) {
                Stream->NotificationEvents[j] = Stream->NotificationEvents[j + 1];
            }
            Stream->NumNotificationEvents--;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

/**
 * @brief Signals all registered period notification events.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtSignalNotification(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
    ULONG i;

    if (Stream == NULL) {
        return;
    }

#ifdef _KERNEL_MODE
    for (i = 0; i < Stream->NumNotificationEvents; i++) {
        if (Stream->NotificationEvents[i] != NULL) {
            KeSetEvent((PKEVENT)Stream->NotificationEvents[i], 0, FALSE);
        }
    }
#else
    UNREFERENCED_PARAMETER(i);
#endif
}

/**
 * @brief Configures BAR0 HD-A stream registers for host audio DMA transfer.
 *
 * Sequence:
 *  1. Resets the stream engine (asserts then deasserts SRST in SD_CTL).
 *  2. Decouples host DMA directly into DSP FIFO via Processing Pipe Control (PPCTL).
 *  3. Programs 64-bit BDL Physical Address (SD_BDLPL and SD_BDLPU).
 *  4. Programs Cyclic Buffer Length (SD_CBL) and Last Valid Index (SD_LVI = 1).
 *  5. Programs computed 16-bit format word (SD_FORMAT).
 *  6. Programs Stream Tag and enables interrupt mask in SD_CTL.
 *  7. Enables stream interrupt and global interrupt in SOF_HDA_INTCTL.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtConfigureHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    if (Stream != NULL && Stream->DeviceContext != NULL && Stream->DeviceContext->Bar0VirtualAddress != NULL) {
        PUCHAR bar0 = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress;
        ULONG streamIndex = Stream->StreamIndex;
        PUCHAR streamBase = bar0 + SOF_HDA_STREAM_REG_OFFSET(streamIndex);

        /* 1. Reset stream engine */
        writel(0x01, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
        KeStallExecutionProcessor(50);
        writel(0x00, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
        KeStallExecutionProcessor(50);

        /* 2. Decouple host DMA to DSP via PPCTL */
        if (Stream->DeviceContext->PpCapOffset) {
            PUCHAR ppctl = bar0 + Stream->DeviceContext->PpCapOffset + SOF_HDA_REG_PP_PPCTL;
            writel(readl(ppctl) | (1 << streamIndex) | SOF_HDA_PPCTL_GPROCEN | SOF_HDA_PPCTL_PIE, ppctl);
        }

        /* 3. Program BDL Physical Address (4KB aligned) */
        writel(Stream->BdlPhysicalAddress.LowPart, streamBase + SOF_HDA_ADSP_REG_SD_BDLPL);
        writel(Stream->BdlPhysicalAddress.HighPart, streamBase + SOF_HDA_ADSP_REG_SD_BDLPU);

        /* 4. Program Cyclic Buffer Length (CBL) */
        writel(Stream->BufferSizeBytes, streamBase + SOF_HDA_ADSP_REG_SD_CBL);

        /* 5. Program Last Valid Index (LVI = entries - 1) */
        writel(1, streamBase + SOF_HDA_ADSP_REG_SD_LVI);

        /* 5b. Program Stream Format */
        {
            USHORT hdaFmt = SofHdaComputeFormatWord(
                Stream->Format.SamplingFrequency,
                Stream->Format.ValidBitDepth,
                Stream->Format.ChannelsCount
            );
            writew(hdaFmt, streamBase + SOF_HDA_ADSP_REG_SD_FORMAT);
        }

        /* 6. Program Stream Tag and Interrupt Mask */
        writel(((Stream->StreamTag & 0x0F) << 20) | SOF_HDA_CL_DMA_SD_INT_MASK,
               streamBase + SOF_HDA_ADSP_REG_SD_CTL);

        /* 7. Enable stream interrupt & Global Interrupt in SOF_HDA_INTCTL */
        writel(readl(bar0 + SOF_HDA_INTCTL) | (1 << streamIndex) | SOF_HDA_INT_GLOBAL_EN | SOF_HDA_INT_CTRL_EN,
               bar0 + SOF_HDA_INTCTL);
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Starts the host HD-A stream DMA engine.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStartHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    if (Stream != NULL && Stream->DeviceContext != NULL && Stream->DeviceContext->Bar0VirtualAddress != NULL) {
        PUCHAR streamBase = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress +
                            SOF_HDA_STREAM_REG_OFFSET(Stream->StreamIndex);
        ULONG ctl = readl(streamBase + SOF_HDA_ADSP_REG_SD_CTL);
        writel(ctl | SOF_HDA_SD_CTL_DMA_START | SOF_HDA_CL_DMA_SD_INT_MASK, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Halts the host HD-A stream DMA engine by clearing SD_CTL.DMA_START.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStopHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    if (Stream != NULL && Stream->DeviceContext != NULL && Stream->DeviceContext->Bar0VirtualAddress != NULL) {
        PUCHAR streamBase = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress +
                            SOF_HDA_STREAM_REG_OFFSET(Stream->StreamIndex);
        ULONG ctl = readl(streamBase + SOF_HDA_ADSP_REG_SD_CTL);
        writel(ctl & ~SOF_HDA_SD_CTL_DMA_START, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Resets the host HD-A stream DMA engine (asserts SRST, stalls, then clears SRST).
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtResetHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    if (Stream != NULL && Stream->DeviceContext != NULL && Stream->DeviceContext->Bar0VirtualAddress != NULL) {
        PUCHAR streamBase = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress +
                            SOF_HDA_STREAM_REG_OFFSET(Stream->StreamIndex);
        writel(0x01, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
        KeStallExecutionProcessor(50);
        writel(0x00, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/* =========================================================================
 * Link Stream Engine (PPLC) Operations
 * ========================================================================= */

/**
 * @brief Computes MMIO base address for a stream's Processing Pipe Link Control (PPLC) registers.
 *
 * Formula:
 *   pplcOffset = PpCapOffset + SOF_HDA_PPLC_BASE +
 *                (totalStreams * SOF_HDA_PPLC_MULTI) +
 *                (streamIndex * SOF_HDA_PPLC_INTERVAL);
 *
 * @param[in] Stream Pointer to stream instance.
 * @return MMIO address of PPLC registers, or NULL if not supported.
 */
static PUCHAR SofWaveRtGetPplcAddress(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    if (Stream != NULL && Stream->DeviceContext != NULL && Stream->DeviceContext->Bar0VirtualAddress != NULL) {
        if (Stream->EndpointType == SOF_ENDPOINT_DMIC_PDM || Stream->EndpointType == SOF_ENDPOINT_I2S_SSP2) {
            return NULL;
        }
        PSOF_DEVICE_CONTEXT ctx = Stream->DeviceContext;
        if (ctx->PpCapOffset != 0) {
            PUCHAR bar0 = (PUCHAR)ctx->Bar0VirtualAddress;
            ULONG totalStreams = ctx->NumCaptureStreams + ctx->NumPlaybackStreams;
            ULONG pplcOffset = ctx->PpCapOffset + SOF_HDA_PPLC_BASE +
                               (totalStreams * SOF_HDA_PPLC_MULTI) +
                               (Stream->StreamIndex * SOF_HDA_PPLC_INTERVAL);
            return bar0 + pplcOffset;
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
    return NULL;
}

/**
 * @brief Resets the Processing Pipe Link Control (PPLC) stream DMA engine.
 *
 * Stops link DMA RUN, asserts STRST, polls for assertion, then clears STRST and polls.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtResetLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl;
        ULONG timeout;

        /* Stop link DMA first */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl & ~SOF_HDA_PPLCCTL_RUN, pplc + SOF_HDA_REG_PPLCCTL);

        /* Assert STRST */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl | SOF_HDA_PPLCCTL_STRST, pplc + SOF_HDA_REG_PPLCCTL);
        timeout = 50;
        while (--timeout) {
            KeStallExecutionProcessor(5);
            if (readl(pplc + SOF_HDA_REG_PPLCCTL) & SOF_HDA_PPLCCTL_STRST) break;
        }

        /* Deassert STRST */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl & ~SOF_HDA_PPLCCTL_STRST, pplc + SOF_HDA_REG_PPLCCTL);
        timeout = 50;
        while (--timeout) {
            KeStallExecutionProcessor(5);
            if (!(readl(pplc + SOF_HDA_REG_PPLCCTL) & SOF_HDA_PPLCCTL_STRST)) break;
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Configures PPLC registers and Multi-Link (ML) capability for link streaming.
 *
 * Sequence:
 *  1. Resets the link stream.
 *  2. Programs StreamTag into PPLCCTL.
 *  3. Programs 16-bit audio format word into PPLCFMT.
 *  4. For HDMI endpoints, powers up the link (MLCTL_SPA) and asserts the stream ID
 *     in the Link Output Stream ID Valid (LOSIDV) register.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtConfigureLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl;

        /* Reset the link stream */
        SofWaveRtResetLinkStream(Stream);

        /* Program StreamTag into PPLCCTL */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        ctl = (ctl & ~SOF_HDA_PPLCCTL_STRM_MASK) |
              ((Stream->StreamTag & 0x0F) << SOF_HDA_PPLCCTL_STRM_SHIFT);
        writel(ctl, pplc + SOF_HDA_REG_PPLCCTL);

        /* Program Format based on stream audio parameters */
        {
            USHORT pplcFmt = SofHdaComputeFormatWord(
                Stream->Format.SamplingFrequency,
                Stream->Format.ValidBitDepth,
                Stream->Format.ChannelsCount
            );
            writew(pplcFmt, pplc + SOF_HDA_REG_PPLCFMT);
        }

        /* If ML capability is present, configure link (only for HDMI and HDA endpoints) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->MlCapOffset != 0 &&
            Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG linkIndex = 0;
            if (Stream->EndpointType == SOF_ENDPOINT_HDMI_1) linkIndex = 1;
            else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_2) linkIndex = 2;
            else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_3) linkIndex = 3;

            PUCHAR bar0 = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress;
            PUCHAR ml_addr = bar0 + Stream->DeviceContext->MlCapOffset + SOF_HDA_ML_BASE +
                             (linkIndex * SOF_HDA_ML_INTERVAL);

            /* Power up link (SPA bit 16) */
            writel(readl(ml_addr + SOF_HDA_REG_ML_LCTL) | SOF_HDA_MLCTL_SPA, ml_addr + SOF_HDA_REG_ML_LCTL);

            /* If output (playback), enable stream ID in LOSIDV */
            if (!Stream->IsCapture) {
                writew(readw(ml_addr + SOF_HDA_REG_ML_LOSIDV) | (1 << Stream->StreamTag),
                       ml_addr + SOF_HDA_REG_ML_LOSIDV);
            }
        }

        {
            char bmsg[160];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] LinkStream configured: idx=%u, pplc=%p, ctl=0x%08x, fmt=0x%04x\n",
                Stream->StreamIndex, pplc, readl(pplc + SOF_HDA_REG_PPLCCTL), readw(pplc + SOF_HDA_REG_PPLCFMT));
            SofLogBreadcrumb(bmsg);
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Starts link DMA transfer by setting SOF_HDA_PPLCCTL_RUN.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStartLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl | SOF_HDA_PPLCCTL_RUN, pplc + SOF_HDA_REG_PPLCCTL);
        {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] StartLinkStream: idx=%u, pplc=%p, ctl=0x%08x\n",
                Stream->StreamIndex, pplc, readl(pplc + SOF_HDA_REG_PPLCCTL));
            SofLogBreadcrumb(bmsg);
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Stops link DMA transfer by clearing SOF_HDA_PPLCCTL_RUN.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStopLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl & ~SOF_HDA_PPLCCTL_RUN, pplc + SOF_HDA_REG_PPLCCTL);
        {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] StopLinkStream: idx=%u, pplc=%p, ctl=0x%08x\n",
                Stream->StreamIndex, pplc, readl(pplc + SOF_HDA_REG_PPLCCTL));
            SofLogBreadcrumb(bmsg);
        }
    }
    if (Stream != NULL && !Stream->IsCapture && Stream->DeviceContext != NULL && Stream->DeviceContext->MlCapOffset != 0 &&
        Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
        ULONG linkIndex = 0;
        if (Stream->EndpointType == SOF_ENDPOINT_HDMI_1) linkIndex = 1;
        else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_2) linkIndex = 2;
        else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_3) linkIndex = 3;

        PUCHAR bar0 = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress;
        PUCHAR ml_addr = bar0 + Stream->DeviceContext->MlCapOffset + SOF_HDA_ML_BASE +
                         (linkIndex * SOF_HDA_ML_INTERVAL);
        writew(readw(ml_addr + SOF_HDA_REG_ML_LOSIDV) & ~(1 << Stream->StreamTag),
               ml_addr + SOF_HDA_REG_ML_LOSIDV);
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/* =========================================================================
 * Audio Control Operations
 * ========================================================================= */

/**
 * @brief Sets stream volume level in 1/65536 dB units and sends IPC4 volume command.
 *
 * Clamps volume to [-96 dB, 0 dB]. If stream is currently active and unmuted,
 * transmits an IPC4 LARGE_CONFIG_SET message to the instantiated Gain module in DSP.
 *
 * @param[in] Stream   Pointer to stream instance.
 * @param[in] Channel  Channel index (0: left, 1: right, -1: master).
 * @param[in] VolumeDb Volume level in 1/65536 dB units.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofWaveRtSetVolume(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _In_ LONG VolumeDb
)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG chMask;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Clamp volume to [-96 dB, 0 dB] */
    if (VolumeDb > SOF_VOLUME_MAX_DB_UNITS) {
        VolumeDb = SOF_VOLUME_MAX_DB_UNITS;
    }
    if (VolumeDb < SOF_VOLUME_MIN_DB_UNITS) {
        VolumeDb = SOF_VOLUME_MIN_DB_UNITS;
    }

    if (Channel == 0) {
        Stream->VolumeLevelDb[0] = VolumeDb;
        chMask = 0;
    } else if (Channel == 1) {
        Stream->VolumeLevelDb[1] = VolumeDb;
        chMask = 1;
    } else {
        /* Master / all channels */
        Stream->VolumeLevelDb[0] = VolumeDb;
        Stream->VolumeLevelDb[1] = VolumeDb;
        chMask = SOF_IPC4_GAIN_ALL_CHANNELS_MASK;
    }

    /* If stream is active and not muted, send IPC4 to Gain module */
    if (Stream->IsActive && !Stream->MasterMute && Stream->GainInstance != 0xFFFFFFFF) {
        status = SofIpc4SetVolumeLevel(Stream->DeviceContext, Stream->GainInstance, chMask, VolumeDb);
    }

    /* Update cached default in DeviceContext */
    if (Stream->DeviceContext != NULL && Stream->EndpointType < SOF_ENDPOINT_COUNT) {
        if (Channel == 0 || Channel == -1) {
            Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][0] = Stream->VolumeLevelDb[0];
        }
        if (Channel == 1 || Channel == -1) {
            Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][1] = Stream->VolumeLevelDb[1];
        }
    }

    return status;
}

/**
 * @brief Retrieves cached volume level for the specified channel.
 *
 * @param[in]  Stream      Pointer to stream instance.
 * @param[in]  Channel     Channel index (0 or 1).
 * @param[out] VolumeDbOut Receives volume level in 1/65536 dB units.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtGetVolume(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _Out_ PLONG VolumeDbOut
)
{
    if (Stream == NULL || VolumeDbOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Channel == 1) {
        *VolumeDbOut = Stream->VolumeLevelDb[1];
    } else {
        *VolumeDbOut = Stream->VolumeLevelDb[0];
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Sets master mute status and dispatches IPC4 mute command to DSP Gain module.
 *
 * @param[in] Stream Pointer to stream instance.
 * @param[in] Mute   TRUE to mute; FALSE to unmute.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofWaveRtSetMute(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ BOOLEAN Mute
)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Stream->MasterMute = Mute;
    Stream->IsMuted[0] = Mute;
    Stream->IsMuted[1] = Mute;

    /* If stream is active, send IPC4 to Gain module */
    if (Stream->IsActive && Stream->GainInstance != 0xFFFFFFFF) {
        LONG targetDb = Stream->VolumeLevelDb[0];
        status = SofIpc4SetMute(Stream->DeviceContext, Stream->GainInstance,
                                SOF_IPC4_GAIN_ALL_CHANNELS_MASK, Mute, targetDb);
    }

    /* Update cached default in DeviceContext */
    if (Stream->DeviceContext != NULL && Stream->EndpointType < SOF_ENDPOINT_COUNT) {
        Stream->DeviceContext->DefaultMute[Stream->EndpointType] = Mute;
    }

    return status;
}

/**
 * @brief Retrieves current master mute state.
 *
 * @param[in]  Stream  Pointer to stream instance.
 * @param[out] MuteOut Receives TRUE if muted, FALSE otherwise.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtGetMute(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PBOOLEAN MuteOut
)
{
    if (Stream == NULL || MuteOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *MuteOut = Stream->MasterMute;
    return STATUS_SUCCESS;
}

/**
 * @brief Retrieves peak meter level for Windows Audio mixer display.
 *
 * If muted, stopped, or paused, returns -96 dB (silence). Otherwise, returns
 * current channel volume level as peak representation.
 *
 * @param[in]  Stream           Pointer to stream instance.
 * @param[in]  Channel          Channel index (0 or 1).
 * @param[out] PeakMeterDbOut   Receives peak level in 1/65536 dB units.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtGetPeakMeter(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _Out_ PLONG PeakMeterDbOut
)
{
    if (Stream == NULL || PeakMeterDbOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* If stopped, paused, or muted: return silence (-96 dB) */
    if (!Stream->IsActive || Stream->State != KSSTATE_RUN || Stream->MasterMute) {
        *PeakMeterDbOut = SOF_VOLUME_MIN_DB_UNITS;
        return STATUS_SUCCESS;
    }

    /* When running and unmuted: return current volume level as peak */
    if (Channel == 1) {
        *PeakMeterDbOut = Stream->VolumeLevelDb[1];
    } else {
        *PeakMeterDbOut = Stream->VolumeLevelDb[0];
    }

    return STATUS_SUCCESS;
}
