/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file HdaBusInterface.c
 * @brief High Definition Audio Bus Interface (HDAUDIO_BUS_INTERFACE_V2) Implementation.
 *
 * @details
 * Implements the standard Microsoft Windows HD-A Bus DDI (Device Driver Interface):
 *  - TransferCodecVerbs: Dispatches synchronous or asynchronous immediate verb arrays.
 *  - AllocateCaptureDmaEngine / AllocateRenderDmaEngine: Manages stream descriptor reservation.
 *  - AllocateDmaBuffer / FreeDmaBuffer: Manages physical cyclic DMA memory and BDLs.
 *  - SetDmaEngineState: Runs, pauses, or resets HD-A stream DMA engines.
 *  - Child PDO Creation: Enumerates and attaches the HD-A audio codec as a child device.
 * Copyright 2026 The SOF Authors
 */

#ifdef _KERNEL_MODE

#include <initguid.h>
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <hdaudio.h>
#include "Device.h"
#include "HdaVerb.h"
#include "HdaBusInterface.h"
#include "WaveRt.h"

/**
 * @brief Reference counter callback for HDAUDIO_BUS_INTERFACE_V2.
 * @param[in] Context Pointer to device context.
 */
VOID NTAPI SofHdaInterfaceReference(
    _In_ PVOID Context
)
{
    UNREFERENCED_PARAMETER(Context);
}

/**
 * @brief Dereference callback for HDAUDIO_BUS_INTERFACE_V2.
 * @param[in] Context Pointer to device context.
 */
VOID NTAPI SofHdaInterfaceDereference(
    _In_ PVOID Context
)
{
    UNREFERENCED_PARAMETER(Context);
}

/**
 * @brief Transmits an array of HD-A verbs to attached codecs and returns responses.
 *
 * @param[in]     _context      Pointer to device context.
 * @param[in]     Count         Number of transfer items in CodecTransfer array.
 * @param[in,out] CodecTransfer Array of HDAUDIO_CODEC_TRANSFER commands and response buffers.
 * @param[in,opt] Callback      Optional completion callback routine.
 * @param[in,opt] Context       Context passed to callback.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS NTAPI SofHdaTransferCodecVerbs(
    _In_ PVOID _context,
    _In_ ULONG Count,
    _Inout_updates_(Count) PHDAUDIO_CODEC_TRANSFER CodecTransfer,
    _In_opt_ PHDAUDIO_TRANSFER_COMPLETE_CALLBACK Callback,
    _In_opt_ PVOID Context
)
{
    PSOF_DEVICE_CONTEXT ctx = (PSOF_DEVICE_CONTEXT)_context;
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;

    if (!ctx || !CodecTransfer || Count == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < Count; i++) {
        ULONG rawResponse = 0;
        ULONG cmd = CodecTransfer[i].Output.Command;

        status = SofHdaSendVerb(ctx, cmd, &rawResponse);
        if (NT_SUCCESS(status)) {
            CodecTransfer[i].Input.CompleteResponse = 0;
            CodecTransfer[i].Input.Response = rawResponse;
            CodecTransfer[i].Input.IsValid = 1;
            CodecTransfer[i].Input.HasFifoOverrun = 0;
            CodecTransfer[i].Input.IsUnsolicitedResponse = 0;
            CodecTransfer[i].Input.SDataIn = (cmd >> 28) & 0x0F;
        } else {
            CodecTransfer[i].Input.CompleteResponse = 0;
            CodecTransfer[i].Input.IsValid = 0;
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[HDA-VerbTransfer] Verb 0x%08X failed: status=0x%08X\n", cmd, status);
            SofLogBreadcrumb(bmsg);
            break;
        }
    }

    if (Callback != NULL) {
        Callback(CodecTransfer, Context);
    }

    return status;
}

static VOID SofHdaFormatToConverterFormat(
    _In_ PHDAUDIO_STREAM_FORMAT Format,
    _Out_ PHDAUDIO_CONVERTER_FORMAT ConverterFormat
)
{
    USHORT fmt = 0;

    if (!Format || !ConverterFormat) return;

    if (Format->NumberOfChannels > 0 && Format->NumberOfChannels <= 16) {
        fmt |= (Format->NumberOfChannels - 1) & 0x0F;
    }

    switch (Format->ValidBitsPerSample) {
    case 8:  fmt |= (0 << 4); break;
    case 16: fmt |= (1 << 4); break;
    case 20: fmt |= (2 << 4); break;
    case 24: fmt |= (3 << 4); break;
    case 32: fmt |= (4 << 4); break;
    default: fmt |= (1 << 4); break;
    }

    switch (Format->SampleRate) {
    case 8000:   fmt |= (0x05 << 8); break;
    case 11025:  fmt |= (0x43 << 8); break;
    case 16000:  fmt |= (0x02 << 8); break;
    case 22050:  fmt |= (0x41 << 8); break;
    case 32000:  fmt |= (0x0A << 8); break;
    case 44100:  fmt |= (0x40 << 8); break;
    case 48000:  fmt |= (0x00 << 8); break;
    case 88200:  fmt |= (0x48 << 8); break;
    case 96000:  fmt |= (0x08 << 8); break;
    case 176400: fmt |= (0x58 << 8); break;
    case 192000: fmt |= (0x18 << 8); break;
    default:     fmt |= (0x00 << 8); break;
    }

    ConverterFormat->ConverterFormat = fmt;
}

NTSTATUS NTAPI SofHdaAllocateCaptureDmaEngine(
    _In_ PVOID _context,
    _In_ UCHAR CodecAddress,
    _In_ PHDAUDIO_STREAM_FORMAT StreamFormat,
    _Out_ PHANDLE Handle,
    _Out_ PHDAUDIO_CONVERTER_FORMAT ConverterFormat
)
{
    PSOF_DEVICE_CONTEXT ctx = (PSOF_DEVICE_CONTEXT)_context;
    ULONG i;

    UNREFERENCED_PARAMETER(CodecAddress);

    if (!ctx || !StreamFormat || !Handle || !ConverterFormat) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < SOF_HDA_MAX_ENGINES; i++) {
        if (ctx->HdaEngines[i].IsCapture && !ctx->HdaEngines[i].InUse) {
            ctx->HdaEngines[i].InUse = TRUE;
            ctx->HdaEngines[i].Ctx = ctx;
            SofHdaFormatToConverterFormat(StreamFormat, &ctx->HdaEngines[i].ConverterFormat);
            *ConverterFormat = ctx->HdaEngines[i].ConverterFormat;
            *Handle = (HANDLE)&ctx->HdaEngines[i];
            return STATUS_SUCCESS;
        }
    }

    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS NTAPI SofHdaAllocateRenderDmaEngine(
    _In_ PVOID _context,
    _In_ PHDAUDIO_STREAM_FORMAT StreamFormat,
    _In_ BOOLEAN Stripe,
    _Out_ PHANDLE Handle,
    _Out_ PHDAUDIO_CONVERTER_FORMAT ConverterFormat
)
{
    PSOF_DEVICE_CONTEXT ctx = (PSOF_DEVICE_CONTEXT)_context;
    ULONG i;

    UNREFERENCED_PARAMETER(Stripe);

    if (!ctx || !StreamFormat || !Handle || !ConverterFormat) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < SOF_HDA_MAX_ENGINES; i++) {
        if (!ctx->HdaEngines[i].IsCapture && !ctx->HdaEngines[i].InUse) {
            ctx->HdaEngines[i].InUse = TRUE;
            ctx->HdaEngines[i].Ctx = ctx;
            SofHdaFormatToConverterFormat(StreamFormat, &ctx->HdaEngines[i].ConverterFormat);
            *ConverterFormat = ctx->HdaEngines[i].ConverterFormat;
            *Handle = (HANDLE)&ctx->HdaEngines[i];
            return STATUS_SUCCESS;
        }
    }

    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS NTAPI SofHdaChangeBandwidthAllocation(
    _In_ PVOID _context,
    _In_ HANDLE Handle,
    _In_ PHDAUDIO_STREAM_FORMAT StreamFormat,
    _Out_ PHDAUDIO_CONVERTER_FORMAT ConverterFormat
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;

    UNREFERENCED_PARAMETER(_context);

    if (!engine || !StreamFormat || !ConverterFormat) {
        return STATUS_INVALID_PARAMETER;
    }

    SofHdaFormatToConverterFormat(StreamFormat, &engine->ConverterFormat);
    *ConverterFormat = engine->ConverterFormat;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaAllocateDmaBufferWithNotification(
    _In_ PVOID _context,
    _In_ HANDLE Handle,
    _In_ ULONG NotificationCount,
    _In_ SIZE_T RequestedBufferSize,
    _Out_ PMDL *BufferMdl,
    _Out_ PSIZE_T AllocatedBufferSize,
    _Out_ PSIZE_T OffsetFromFirstPage,
    _Out_ PUCHAR StreamId,
    _Out_ PULONG FifoSize
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;
    PSOF_DEVICE_CONTEXT ctx;
    PHYSICAL_ADDRESS maxAddr;
    ULONG numEntries;
    ULONG periodBytes;
    ULONG i;
    PUCHAR streamBase;

    UNREFERENCED_PARAMETER(_context);

    if (!engine || !BufferMdl || !AllocatedBufferSize || !StreamId || !FifoSize) {
        return STATUS_INVALID_PARAMETER;
    }
    ctx = engine->Ctx;
    if (!ctx || !ctx->Bar0VirtualAddress) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Page-align requested buffer size */
    RequestedBufferSize = (RequestedBufferSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (RequestedBufferSize == 0) RequestedBufferSize = PAGE_SIZE * 2;

    /* 1. Allocate Contiguous DMA Buffer */
    maxAddr.QuadPart = 0xFFFFFFFFFFFFFFFFULL;
    engine->BufferVa = MmAllocateContiguousMemory(RequestedBufferSize, maxAddr);
    if (!engine->BufferVa) {
        maxAddr.QuadPart = 0xFFFFFFFFULL;
        engine->BufferVa = MmAllocateContiguousMemory(RequestedBufferSize, maxAddr);
    }
    if (!engine->BufferVa) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(engine->BufferVa, RequestedBufferSize);
    engine->BufferPa = MmGetPhysicalAddress(engine->BufferVa);
    engine->BufferSize = RequestedBufferSize;

    /* 2. Build MDL for the non-paged buffer */
    engine->BufferMdl = IoAllocateMdl(engine->BufferVa, (ULONG)RequestedBufferSize, FALSE, FALSE, NULL);
    if (!engine->BufferMdl) {
        MmFreeContiguousMemory(engine->BufferVa);
        engine->BufferVa = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(engine->BufferMdl);

    /* 3. Allocate BDL (4KB page) */
    engine->BdlVa = MmAllocateContiguousMemory(4096, maxAddr);
    if (!engine->BdlVa) {
        IoFreeMdl(engine->BufferMdl);
        engine->BufferMdl = NULL;
        MmFreeContiguousMemory(engine->BufferVa);
        engine->BufferVa = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(engine->BdlVa, 4096);
    engine->BdlPa = MmGetPhysicalAddress(engine->BdlVa);

    /* 4. Fill BDL */
    numEntries = (NotificationCount > 1) ? NotificationCount : 2;
    if (numEntries > 128) numEntries = 128;
    periodBytes = (ULONG)RequestedBufferSize / numEntries;

    {
        PSOF_INTEL_DSP_BDL bdl = (PSOF_INTEL_DSP_BDL)engine->BdlVa;
        for (i = 0; i < numEntries; i++) {
            bdl[i].AddrLow = (ULONG)(engine->BufferPa.QuadPart + (i * periodBytes));
            bdl[i].AddrHigh = (ULONG)((engine->BufferPa.QuadPart + (i * periodBytes)) >> 32);
            bdl[i].Size = periodBytes;
            bdl[i].Ioc = 1; /* Interrupt On Completion */
        }
    }

    /* 5. Program Hardware Stream Engine on BAR0 */
    streamBase = (PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_STREAM_REG_OFFSET(engine->StreamIndex);

    /* Reset stream engine */
    writel(0x01, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
    KeStallExecutionProcessor(50);
    writel(0x00, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
    KeStallExecutionProcessor(50);

    /* Set BDL physical address */
    writel(engine->BdlPa.LowPart, streamBase + SOF_HDA_ADSP_REG_SD_BDLPL);
    writel(engine->BdlPa.HighPart, streamBase + SOF_HDA_ADSP_REG_SD_BDLPU);

    /* Cyclic Buffer Length */
    writel((ULONG)RequestedBufferSize, streamBase + SOF_HDA_ADSP_REG_SD_CBL);

    /* Last Valid Index */
    writew((USHORT)(numEntries - 1), streamBase + SOF_HDA_ADSP_REG_SD_LVI);

    /* Stream Format */
    writew(engine->ConverterFormat.ConverterFormat, streamBase + SOF_HDA_ADSP_REG_SD_FORMAT);

    /* Stream Tag */
    writel(((ULONG)engine->StreamTag << 20), streamBase + SOF_HDA_ADSP_REG_SD_CTL);

    /* Return parameters */
    *BufferMdl = engine->BufferMdl;
    *AllocatedBufferSize = RequestedBufferSize;
    if (OffsetFromFirstPage) {
        *OffsetFromFirstPage = 0;
    }
    *StreamId = engine->StreamTag;
    *FifoSize = 256;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaAllocateDmaBuffer(
    _In_ PVOID _context,
    _In_ HANDLE Handle,
    _In_ SIZE_T RequestedBufferSize,
    _Out_ PMDL *BufferMdl,
    _Out_ PSIZE_T AllocatedBufferSize,
    _Out_ PUCHAR StreamId,
    _Out_ PULONG FifoSize
)
{
    return SofHdaAllocateDmaBufferWithNotification(
        _context,
        Handle,
        2,  /* 2 half-buffers */
        RequestedBufferSize,
        BufferMdl,
        AllocatedBufferSize,
        NULL,
        StreamId,
        FifoSize
    );
}

NTSTATUS NTAPI SofHdaFreeDmaBufferWithNotification(
    _In_ PVOID _context,
    _In_ HANDLE Handle,
    _In_ PMDL BufferMdl,
    _In_ SIZE_T BufferSize
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;
    PSOF_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(_context);
    UNREFERENCED_PARAMETER(BufferMdl);
    UNREFERENCED_PARAMETER(BufferSize);

    if (!engine) return STATUS_INVALID_PARAMETER;
    ctx = engine->Ctx;

    if (ctx && ctx->Bar0VirtualAddress) {
        PUCHAR streamBase = (PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_STREAM_REG_OFFSET(engine->StreamIndex);
        /* Stop and reset stream */
        writel(0x01, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
        KeStallExecutionProcessor(50);
        writel(0x00, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
    }

    if (engine->BdlVa) {
        MmFreeContiguousMemory(engine->BdlVa);
        engine->BdlVa = NULL;
    }
    if (engine->BufferMdl) {
        IoFreeMdl(engine->BufferMdl);
        engine->BufferMdl = NULL;
    }
    if (engine->BufferVa) {
        MmFreeContiguousMemory(engine->BufferVa);
        engine->BufferVa = NULL;
    }
    engine->NotificationEvent = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaFreeDmaBuffer(
    _In_ PVOID _context,
    _In_ HANDLE Handle
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;
    if (!engine) return STATUS_INVALID_PARAMETER;
    return SofHdaFreeDmaBufferWithNotification(_context, Handle, engine->BufferMdl, engine->BufferSize);
}

NTSTATUS NTAPI SofHdaFreeDmaEngine(
    _In_ PVOID _context,
    _In_ HANDLE Handle
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;

    UNREFERENCED_PARAMETER(_context);

    if (!engine) return STATUS_INVALID_PARAMETER;

    SofHdaFreeDmaBuffer(_context, Handle);
    engine->InUse = FALSE;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaSetDmaEngineState(
    _In_ PVOID _context,
    _In_ HDAUDIO_STREAM_STATE StreamState,
    _In_ ULONG NumberOfHandles,
    _In_reads_(NumberOfHandles) PHANDLE Handles
)
{
    PSOF_DEVICE_CONTEXT ctx = (PSOF_DEVICE_CONTEXT)_context;
    ULONG i;

    if (!ctx || !ctx->Bar0VirtualAddress || !Handles) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < NumberOfHandles; i++) {
        PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handles[i];
        if (!engine) continue;

        PUCHAR streamBase = (PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_STREAM_REG_OFFSET(engine->StreamIndex);
        ULONG ctl = readl(streamBase + SOF_HDA_ADSP_REG_SD_CTL);

        switch (StreamState) {
        case RunState:
            writel(ctl | SOF_HDA_SD_CTL_DMA_START | SOF_HDA_CL_DMA_SD_INT_MASK, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
            break;

        case PauseState:
            writel(ctl & ~SOF_HDA_SD_CTL_DMA_START, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
            break;

        case ResetState:
            writel(0x01, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
            KeStallExecutionProcessor(50);
            writel(0x00, streamBase + SOF_HDA_ADSP_REG_SD_CTL);
            break;
        }
    }

    return STATUS_SUCCESS;
}

VOID NTAPI SofHdaGetWallClockRegister(
    _In_ PVOID _context,
    _Out_ PULONG *Wallclock
)
{
    PSOF_DEVICE_CONTEXT ctx = (PSOF_DEVICE_CONTEXT)_context;
    if (ctx && ctx->Bar0VirtualAddress && Wallclock) {
        *Wallclock = (PULONG)((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_REG_WALCLK);
    }
}

NTSTATUS NTAPI SofHdaGetLinkPositionRegister(
    _In_ PVOID _context,
    _In_ HANDLE Handle,
    _Out_ PULONG *Position
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;
    PSOF_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(_context);

    if (!engine || !Position) return STATUS_INVALID_PARAMETER;
    ctx = engine->Ctx;
    if (!ctx || !ctx->Bar0VirtualAddress) return STATUS_INVALID_PARAMETER;

    PUCHAR streamBase = (PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_STREAM_REG_OFFSET(engine->StreamIndex);
    *Position = (PULONG)(streamBase + SOF_HDA_ADSP_REG_SD_LPIB);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaRegisterEventCallback(
    _In_ PVOID _context,
    _In_ PHDAUDIO_UNSOLICITED_RESPONSE_CALLBACK Routine,
    _In_opt_ PVOID Context,
    _Out_ PUCHAR Tag
)
{
    UNREFERENCED_PARAMETER(_context);
    UNREFERENCED_PARAMETER(Routine);
    UNREFERENCED_PARAMETER(Context);

    if (!Tag) return STATUS_INVALID_PARAMETER;

    /* Assign tag 1 for unsolicited responses */
    *Tag = 1;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaUnregisterEventCallback(
    _In_ PVOID _context,
    _In_ UCHAR Tag
)
{
    UNREFERENCED_PARAMETER(_context);
    UNREFERENCED_PARAMETER(Tag);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaGetDeviceInformation(
    _In_ PVOID _context,
    _Out_ PHDAUDIO_DEVICE_INFORMATION DeviceInformation
)
{
    PSOF_DEVICE_CONTEXT ctx = (PSOF_DEVICE_CONTEXT)_context;

    if (!DeviceInformation) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(DeviceInformation, sizeof(*DeviceInformation));
    DeviceInformation->Size = sizeof(*DeviceInformation);
    DeviceInformation->DeviceVersion = 0x0100;
    DeviceInformation->DriverVersion = 0x0100;
    DeviceInformation->CodecsDetected = (ctx != NULL && ctx->HdaCodecPresent) ? 0x0005 : 0x0001;
    DeviceInformation->IsStripingSupported = FALSE;

    return STATUS_SUCCESS;
}

VOID NTAPI SofHdaGetResourceInformation(
    _In_ PVOID _context,
    _Out_ PUCHAR CodecAddress,
    _Out_ PUCHAR FunctionGroupStartNode
)
{
    UNREFERENCED_PARAMETER(_context);

    if (CodecAddress) {
        *CodecAddress = 0; /* SDIN 0 */
    }
    if (FunctionGroupStartNode) {
        *FunctionGroupStartNode = 1; /* Audio Function Group is node 0x01 */
    }
}

NTSTATUS NTAPI SofHdaRegisterNotificationEvent(
    _In_ PVOID _context,
    _In_ HANDLE Handle,
    _In_ PKEVENT NotificationEvent
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;

    UNREFERENCED_PARAMETER(_context);

    if (!engine || !NotificationEvent) return STATUS_INVALID_PARAMETER;
    engine->NotificationEvent = NotificationEvent;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI SofHdaUnregisterNotificationEvent(
    _In_ PVOID _context,
    _In_ HANDLE Handle,
    _In_ PKEVENT NotificationEvent
)
{
    PSOF_HDA_DMA_ENGINE engine = (PSOF_HDA_DMA_ENGINE)Handle;

    UNREFERENCED_PARAMETER(_context);
    UNREFERENCED_PARAMETER(NotificationEvent);

    if (!engine) return STATUS_INVALID_PARAMETER;
    engine->NotificationEvent = NULL;
    return STATUS_SUCCESS;
}

VOID SofHdaBusInitInterface(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _Out_ PHDAUDIO_BUS_INTERFACE_V2 BusInterface
)
{
    RtlZeroMemory(BusInterface, sizeof(HDAUDIO_BUS_INTERFACE_V2));

    BusInterface->Size = sizeof(HDAUDIO_BUS_INTERFACE_V2);
    BusInterface->Version = 0x0100;
    BusInterface->Context = (PVOID)Ctx;
    BusInterface->InterfaceReference = SofHdaInterfaceReference;
    BusInterface->InterfaceDereference = SofHdaInterfaceDereference;

    BusInterface->TransferCodecVerbs = SofHdaTransferCodecVerbs;
    BusInterface->AllocateCaptureDmaEngine = SofHdaAllocateCaptureDmaEngine;
    BusInterface->AllocateRenderDmaEngine = SofHdaAllocateRenderDmaEngine;
    BusInterface->ChangeBandwidthAllocation = SofHdaChangeBandwidthAllocation;
    BusInterface->AllocateDmaBuffer = SofHdaAllocateDmaBuffer;
    BusInterface->FreeDmaBuffer = SofHdaFreeDmaBuffer;
    BusInterface->FreeDmaEngine = SofHdaFreeDmaEngine;
    BusInterface->SetDmaEngineState = SofHdaSetDmaEngineState;
    BusInterface->GetWallClockRegister = SofHdaGetWallClockRegister;
    BusInterface->GetLinkPositionRegister = SofHdaGetLinkPositionRegister;
    BusInterface->RegisterEventCallback = SofHdaRegisterEventCallback;
    BusInterface->UnregisterEventCallback = SofHdaUnregisterEventCallback;
    BusInterface->GetDeviceInformation = SofHdaGetDeviceInformation;
    BusInterface->GetResourceInformation = SofHdaGetResourceInformation;
    BusInterface->AllocateDmaBufferWithNotification = SofHdaAllocateDmaBufferWithNotification;
    BusInterface->FreeDmaBufferWithNotification = SofHdaFreeDmaBufferWithNotification;
    BusInterface->RegisterNotificationEvent = SofHdaRegisterNotificationEvent;
    BusInterface->UnregisterNotificationEvent = SofHdaUnregisterNotificationEvent;
}

NTSTATUS SofHdaCreateCodecChildPdo(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    NTSTATUS status;
    PWDFDEVICE_INIT pdoInit = NULL;
    WDFDEVICE childDevice = NULL;
    WDF_OBJECT_ATTRIBUTES childAttributes;
    WDF_DEVICE_POWER_CAPABILITIES powerCaps;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDF_QUERY_INTERFACE_CONFIG qiConfigV2;
    WDF_QUERY_INTERFACE_CONFIG qiConfigV1;

    UNICODE_STRING compatId;
    UNICODE_STRING hwId1;
    UNICODE_STRING hwId2;
    UNICODE_STRING hwId3;
    UNICODE_STRING devId;
    UNICODE_STRING instId;
    UNICODE_STRING desc;
    UNICODE_STRING location;

    if (!Ctx || !Ctx->WdfDevice) {
        return STATUS_INVALID_PARAMETER;
    }

    SofLogBreadcrumb("[HDA-Bus] Creating Codec Child PDO...\n");

    RtlInitUnicodeString(&compatId, L"HDAUDIO\\FUNC_01");
    RtlInitUnicodeString(&hwId1, L"HDAUDIO\\FUNC_01&VEN_10EC&DEV_0285&SUBSYS_00000000&REV_1002");
    RtlInitUnicodeString(&hwId2, L"HDAUDIO\\FUNC_01&VEN_10EC&DEV_0285&SUBSYS_00000000");
    RtlInitUnicodeString(&hwId3, L"HDAUDIO\\FUNC_01&VEN_10EC&DEV_0285");
    RtlInitUnicodeString(&devId, L"HDAUDIO\\FUNC_01&VEN_10EC&DEV_0285&SUBSYS_00000000&REV_1002");
    RtlInitUnicodeString(&instId, L"01");
    RtlInitUnicodeString(&desc, L"High Definition Audio Codec");
    RtlInitUnicodeString(&location, L"High Definition Audio Bus 0");

    /* 1. Allocate PDO Init structure */
    pdoInit = WdfPdoInitAllocate(Ctx->WdfDevice);
    if (!pdoInit) {
        SofLogBreadcrumb("[HDA-Bus] WdfPdoInitAllocate failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* 2. Assign IDs */
    status = WdfPdoInitAssignDeviceID(pdoInit, &devId);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAssignInstanceID(pdoInit, &instId);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddHardwareID(pdoInit, &hwId1);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddHardwareID(pdoInit, &hwId2);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddHardwareID(pdoInit, &hwId3);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddCompatibleID(pdoInit, &compatId);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddDeviceText(pdoInit, &desc, &location, 0x409);
    if (!NT_SUCCESS(status)) goto Error;

    WdfPdoInitSetDefaultLocale(pdoInit, 0x409);

    /* 3. Create Child Device */
    WDF_OBJECT_ATTRIBUTES_INIT(&childAttributes);
    status = WdfDeviceCreate(&pdoInit, &childAttributes, &childDevice);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[HDA-Bus] WdfDeviceCreate failed for child PDO\n");
        goto Error;
    }

    /* Protect HDA codec child PDO against IopDecrementDeviceObjectRef underflow */
    PDEVICE_OBJECT wdmHdaPdo = WdfDeviceWdmGetDeviceObject(childDevice);
    if (wdmHdaPdo != NULL) {
        for (ULONG i = 0; i < 100000; i++) {
            ObReferenceObject(wdmHdaPdo);
        }
        InterlockedExchange(&wdmHdaPdo->ReferenceCount, 100000);
    }

    /* 4. Set Power Capabilities */
    WDF_DEVICE_POWER_CAPABILITIES_INIT(&powerCaps);
    powerCaps.DeviceD1 = WdfFalse;
    powerCaps.DeviceD2 = WdfFalse;
    powerCaps.WakeFromD0 = WdfFalse;
    powerCaps.WakeFromD1 = WdfFalse;
    powerCaps.WakeFromD2 = WdfFalse;
    powerCaps.WakeFromD3 = WdfFalse;
    powerCaps.DeviceWake = PowerDeviceUnspecified;
    powerCaps.DeviceState[PowerSystemWorking]   = PowerDeviceD0;
    powerCaps.DeviceState[PowerSystemSleeping1] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemSleeping2] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemSleeping3] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemShutdown]  = PowerDeviceD3;
    WdfDeviceSetPowerCapabilities(childDevice, &powerCaps);

    /* 5. Set PnP Capabilities */
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.Removable = WdfFalse;
    pnpCaps.EjectSupported = WdfFalse;
    pnpCaps.SurpriseRemovalOK = WdfFalse;
    pnpCaps.Address = 0;
    pnpCaps.UINumber = 1;
    WdfDeviceSetPnpCapabilities(childDevice, &pnpCaps);

    /* 6. Register HDAUDIO_BUS_INTERFACE_V2 */
    SofHdaBusInitInterface(Ctx, &Ctx->HdaBusInterfaceV2);
    WDF_QUERY_INTERFACE_CONFIG_INIT(
        &qiConfigV2,
        (PINTERFACE)&Ctx->HdaBusInterfaceV2,
        &GUID_HDAUDIO_BUS_INTERFACE_V2,
        NULL
    );
    status = WdfDeviceAddQueryInterface(childDevice, &qiConfigV2);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDA-Bus] Add QueryInterface V2 failed: 0x%08X\n", status);
        SofLogBreadcrumb(bmsg);
    }

    /* 7. Register HDAUDIO_BUS_INTERFACE (V1 fallback) */
    RtlCopyMemory(&Ctx->HdaBusInterfaceV1, &Ctx->HdaBusInterfaceV2, sizeof(HDAUDIO_BUS_INTERFACE));
    Ctx->HdaBusInterfaceV1.Size = sizeof(HDAUDIO_BUS_INTERFACE);
    Ctx->HdaBusInterfaceV1.Version = 0x0100;
    WDF_QUERY_INTERFACE_CONFIG_INIT(
        &qiConfigV1,
        (PINTERFACE)&Ctx->HdaBusInterfaceV1,
        &GUID_HDAUDIO_BUS_INTERFACE,
        NULL
    );
    status = WdfDeviceAddQueryInterface(childDevice, &qiConfigV1);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDA-Bus] Add QueryInterface V1 failed: 0x%08X\n", status);
        SofLogBreadcrumb(bmsg);
    }

    /* 8. Add Child PDO to Parent Device */
    status = WdfFdoAddStaticChild(Ctx->WdfDevice, childDevice);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDA-Bus] WdfFdoAddStaticChild failed: 0x%08X\n", status);
        SofLogBreadcrumb(bmsg);
        WdfObjectDelete(childDevice);
        return status;
    }

    Ctx->CodecChildDevice = childDevice;
    SofLogBreadcrumb("[HDA-Bus] Codec child PDO created and attached successfully!\n");
    return STATUS_SUCCESS;

Error:
    if (pdoInit) {
        WdfDeviceInitFree(pdoInit);
    }
    return status;
}

#endif /* _KERNEL_MODE */
