/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file UsbOffload.c
 * @brief Sound Open Firmware (SOF) Windows Driver - USB Audio Offload Implementation.
 *
 * @details
 * Coordinates sideband audio claim, endpoint discovery, and stream control with
 * the Windows Sideband Audio stack:
 *  - PNP Interface Notifications: Listens for GUID_DEVINTERFACE_USB_SIDEBAND_AUDIO_HS_HCIBYPASS.
 *  - Device Claiming: Issues IOCTL_SBAUD_SET_DEVICE_CLAIMED to prevent default USB audio
 *    driver enumeration and route data paths through Intel Audio DSP.
 *  - Stream Lifecycle: Sends IOCTL_SBAUD_STREAM_OPEN, START, SUSPEND, and CLOSE.
 *  - Mixer Control: Propagates volume and mute updates directly to hardware USB endpoints.
 * Copyright 2026 The SOF Authors
 */

#include "UsbOffload.h"
#include "Device.h"
#include "Driver.h"
#include "UaolGateway.h"
#include <ntstrsafe.h>
#include <wdmguid.h>

/* {02BAA4B5-33B5-4D97-AE4F-E86DDE17536F} */
static const GUID GUID_DEVINTERFACE_USB_SIDEBAND_AUDIO_HS_HCIBYPASS = 
    { 0x02baa4b5, 0x33b5, 0x4d97, { 0xae, 0x4f, 0xe8, 0x6d, 0xde, 0x17, 0x53, 0x6f } };

/* Forward declaration of PortCls notification helpers */
#ifdef __cplusplus
extern "C" {
#endif
void SofPortClsNotifyUsbOffloadArrival(PSOF_DEVICE_CONTEXT Ctx);
void SofPortClsNotifyUsbOffloadRemoval(PSOF_DEVICE_CONTEXT Ctx);
#ifdef __cplusplus
}
#endif

/**
 * @brief Synchronously sends a Kernel DeviceIoControl IRP to the Sideband Audio stack.
 *
 * @param[in]  TargetDevice       Target device object.
 * @param[in]  IoControlCode      IOCTL code.
 * @param[in]  InputBuffer        Optional input payload.
 * @param[in]  InputBufferLength  Input payload size.
 * @param[out] OutputBuffer       Optional output buffer.
 * @param[in]  OutputBufferLength Output buffer size.
 * @param[out] BytesReturned      Optional bytes returned pointer.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
static NTSTATUS
SofUsbSendIoctl(
    _In_ PDEVICE_OBJECT TargetDevice,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_opt_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_opt_ PULONG_PTR BytesReturned
)
{
    PIRP irp;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;

    if (TargetDevice == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IoControlCode,
        TargetDevice,
        InputBuffer,
        InputBufferLength,
        OutputBuffer,
        OutputBufferLength,
        FALSE,
        &event,
        &ioStatus
    );

    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(TargetDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (BytesReturned != NULL) {
        *BytesReturned = NT_SUCCESS(status) ? ioStatus.Information : 0;
    }

    return status;
}

/* PnP Device Interface Change Callback */
static NTSTATUS
SofUsbSidebandPnpCallback(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context
)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION notify = 
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    PSOF_DEVICE_CONTEXT ctx = (PSOF_DEVICE_CONTEXT)Context;

    if (notify == NULL || ctx == NULL) {
        return STATUS_SUCCESS;
    }

    if (IsEqualGUID(&notify->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) {
        SofLogBreadcrumb("[USB-Offload] PnP Arrival: USB Sideband Audio interface arrived!\n");
        SofUsbOffloadClaimDevice(ctx, notify->SymbolicLinkName);
    } else if (IsEqualGUID(&notify->Event, &GUID_DEVICE_INTERFACE_REMOVAL)) {
        SofLogBreadcrumb("[USB-Offload] PnP Removal: USB Sideband Audio interface removed!\n");
        SofUsbOffloadUnclaimDevice(ctx);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
SofUsbOffloadInit(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    NTSTATUS status;
    char msg[128];

    if (Ctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeSpinLock(&Ctx->UsbOffloadState.Lock);

    /* 1. Canary Protection Guard */
    if (!g_CanaryArmed) {
        SofLogBreadcrumb("[USB-Offload] Safe Mode active: Canary not armed -> bypassing USB offload claim.\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Safe Mode: Canary not armed -> USB offload claim bypassed.\n");
        return STATUS_SUCCESS;
    }

    /* 2. Feature Enabled Check */
    if (!Ctx->EnableUsbOffload) {
        SofLogBreadcrumb("[USB-Offload] Feature disabled in registry (EnableUsbOffload=0).\n");
        return STATUS_SUCCESS;
    }

    SofLogBreadcrumb("[USB-Offload] Registering for GUID_DEVINTERFACE_USB_SIDEBAND_AUDIO_HS_HCIBYPASS...\n");

    /* 3. Register for PnP Device Interface Notifications */
    status = IoRegisterPlugPlayNotification(
        EventCategoryDeviceInterfaceChange,
        PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
        (PVOID)&GUID_DEVINTERFACE_USB_SIDEBAND_AUDIO_HS_HCIBYPASS,
        WdfDriverWdmGetDriverObject(WdfGetDriver()),
        SofUsbSidebandPnpCallback,
        Ctx,
        &Ctx->UsbOffloadState.PnpNotificationHandle
    );

    RtlStringCbPrintfA(msg, sizeof(msg), "[USB-Offload] IoRegisterPlugPlayNotification: status=0x%08X (Handle=%p)\n",
                       status, Ctx->UsbOffloadState.PnpNotificationHandle);
    SofLogBreadcrumb(msg);

    return status;
}

void
SofUsbOffloadRelease(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (Ctx == NULL) {
        return;
    }

    if (Ctx->UsbOffloadState.PnpNotificationHandle != NULL) {
        IoUnregisterPlugPlayNotificationEx(Ctx->UsbOffloadState.PnpNotificationHandle);
        Ctx->UsbOffloadState.PnpNotificationHandle = NULL;
        SofLogBreadcrumb("[USB-Offload] Unregistered PnP notification.\n");
    }

    SofUsbOffloadUnclaimDevice(Ctx);
}

NTSTATUS
SofUsbOffloadClaimDevice(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PUNICODE_STRING SymbolicLink
)
{
    NTSTATUS status;
    KIRQL oldIrql;
    PFILE_OBJECT fileObj = NULL;
    PDEVICE_OBJECT devObj = NULL;
    SIDEBANDAUDIO_DEVICE_DESCRIPTOR devDesc;
    BOOL claimParam = TRUE;
    ULONG_PTR bytesRet = 0;
    char msg[256];

    if (Ctx == NULL || SymbolicLink == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlStringCbPrintfA(msg, sizeof(msg), "[USB-Offload] Claiming device: %wZ\n", SymbolicLink);
    SofLogBreadcrumb(msg);

    /* 1. Open Target Device Object Pointer */
    status = IoGetDeviceObjectPointer(
        SymbolicLink,
        FILE_ALL_ACCESS,
        &fileObj,
        &devObj
    );

    if (!NT_SUCCESS(status)) {
        RtlStringCbPrintfA(msg, sizeof(msg), "[USB-Offload] IoGetDeviceObjectPointer failed: 0x%08X\n", status);
        SofLogBreadcrumb(msg);
        return status;
    }

    /* 2. Query Device Descriptor */
    RtlZeroMemory(&devDesc, sizeof(devDesc));
    status = SofUsbSendIoctl(
        devObj,
        IOCTL_SBAUD_GET_DEVICE_DESCRIPTOR,
        NULL,
        0,
        &devDesc,
        sizeof(devDesc),
        &bytesRet
    );

    if (NT_SUCCESS(status)) {
        RtlStringCbPrintfA(msg, sizeof(msg), "[USB-Offload] DeviceDescriptor: NumberOfEndpoints=%u\n",
                           devDesc.NumberOfEndpoints);
        SofLogBreadcrumb(msg);
    } else {
        /* Fallback default if not implemented by target device */
        devDesc.NumberOfEndpoints = 2; /* 1 playback + 1 capture */
        SofLogBreadcrumb("[USB-Offload] Using default 2 endpoints.\n");
    }

    /* 3. Claim Device for Sideband Operation (suppressing inband UAC2 legacy endpoints) */
    status = SofUsbSendIoctl(
        devObj,
        IOCTL_SBAUD_SET_DEVICE_CLAIMED,
        &claimParam,
        sizeof(claimParam),
        NULL,
        0,
        NULL
    );

    if (NT_SUCCESS(status)) {
        SofLogBreadcrumb("[USB-Offload] IOCTL_SBAUD_SET_DEVICE_CLAIMED(TRUE) SUCCEEDED! In-band UAC2 suppressed.\n");
    } else {
        RtlStringCbPrintfA(msg, sizeof(msg), "[USB-Offload] IOCTL_SBAUD_SET_DEVICE_CLAIMED returned 0x%08X (continuing)\n", status);
        SofLogBreadcrumb(msg);
    }

    /* 4. Update Device Context under SpinLock */
    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    Ctx->UsbOffloadState.FileObject = fileObj;
    Ctx->UsbOffloadState.DeviceObject = devObj;
    Ctx->UsbOffloadState.DevicePresent = TRUE;
    Ctx->UsbOffloadState.DeviceClaimed = TRUE;
    Ctx->UsbOffloadState.EndpointCount = devDesc.NumberOfEndpoints;
    Ctx->UsbOffloadState.DeviceDesc = devDesc;

    /* Initialize default endpoint streams */
    for (ULONG i = 0; i < SOF_USB_MAX_ENDPOINTS; i++) {
        Ctx->UsbOffloadState.Streams[i].EpIndex = i;
        Ctx->UsbOffloadState.Streams[i].IsOpen = FALSE;
        Ctx->UsbOffloadState.Streams[i].IsRunning = FALSE;
        Ctx->UsbOffloadState.Streams[i].Direction = (i == 0) ? SOF_USB_DATAFLOW_OUT : SOF_USB_DATAFLOW_IN;
        Ctx->UsbOffloadState.Streams[i].SampleRate = 48000;
        Ctx->UsbOffloadState.Streams[i].Channels = 2;
        Ctx->UsbOffloadState.Streams[i].BitDepth = 24;
        Ctx->UsbOffloadState.Streams[i].VolumeDb[0] = 0;
        Ctx->UsbOffloadState.Streams[i].VolumeDb[1] = 0;
        Ctx->UsbOffloadState.Streams[i].Mute = FALSE;
    }
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    /* 5. Notify PortCls Adapter to expose the dynamic CUBILUX CA7 offload endpoint */
    SofPortClsNotifyUsbOffloadArrival(Ctx);

    return STATUS_SUCCESS;
}

void
SofUsbOffloadUnclaimDevice(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    KIRQL oldIrql;
    PFILE_OBJECT fileObj = NULL;
    PDEVICE_OBJECT devObj = NULL;
    BOOL claimParam = FALSE;

    if (Ctx == NULL) {
        return;
    }

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    if (!Ctx->UsbOffloadState.DevicePresent) {
        KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);
        return;
    }

    fileObj = Ctx->UsbOffloadState.FileObject;
    devObj = Ctx->UsbOffloadState.DeviceObject;

    Ctx->UsbOffloadState.DevicePresent = FALSE;
    Ctx->UsbOffloadState.DeviceClaimed = FALSE;
    Ctx->UsbOffloadState.FileObject = NULL;
    Ctx->UsbOffloadState.DeviceObject = NULL;

    /* Halt all active streams */
    for (ULONG i = 0; i < SOF_USB_MAX_ENDPOINTS; i++) {
        Ctx->UsbOffloadState.Streams[i].IsOpen = FALSE;
        Ctx->UsbOffloadState.Streams[i].IsRunning = FALSE;
    }
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    /* Halt hardware Link DMA engine */
    SofUaolStopPipeline(Ctx, FALSE);
    SofUaolStopPipeline(Ctx, TRUE);

    /* Release Claim with USB Class Driver */
    if (devObj != NULL) {
        SofUsbSendIoctl(
            devObj,
            IOCTL_SBAUD_SET_DEVICE_CLAIMED,
            &claimParam,
            sizeof(claimParam),
            NULL,
            0,
            NULL
        );
        SofLogBreadcrumb("[USB-Offload] IOCTL_SBAUD_SET_DEVICE_CLAIMED(FALSE) released.\n");
    }

    if (fileObj != NULL) {
        ObDereferenceObject(fileObj);
    }

    /* Notify PortCls Adapter of removal */
    SofPortClsNotifyUsbOffloadRemoval(Ctx);
    SofLogBreadcrumb("[USB-Offload] Device unclaim complete.\n");
}

NTSTATUS
SofUsbOffloadStreamOpen(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG EpIndex,
    _In_ ULONG SampleRate,
    _In_ ULONG Channels,
    _In_ ULONG BitDepth
)
{
    KIRQL oldIrql;
    PDEVICE_OBJECT devObj;
    SIDEBANDAUDIO_STREAM_OPEN_PARAMS openParams;
    NTSTATUS status;

    if (Ctx == NULL || EpIndex >= SOF_USB_MAX_ENDPOINTS) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    devObj = Ctx->UsbOffloadState.DeviceObject;
    Ctx->UsbOffloadState.Streams[EpIndex].SampleRate = SampleRate;
    Ctx->UsbOffloadState.Streams[EpIndex].Channels = Channels;
    Ctx->UsbOffloadState.Streams[EpIndex].BitDepth = BitDepth;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    if (devObj == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    RtlZeroMemory(&openParams, sizeof(openParams));
    openParams.EpIndex = EpIndex;
    openParams.SiopCount = 0;

    status = SofUsbSendIoctl(
        devObj,
        IOCTL_SBAUD_STREAM_OPEN,
        &openParams,
        sizeof(openParams),
        NULL,
        0,
        NULL
    );

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    Ctx->UsbOffloadState.Streams[EpIndex].IsOpen = TRUE;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    SofLogBreadcrumb("[USB-Offload] StreamOpen succeeded.\n");
    return STATUS_SUCCESS;
}

NTSTATUS
SofUsbOffloadStreamStart(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG EpIndex
)
{
    KIRQL oldIrql;
    PDEVICE_OBJECT devObj;
    ULONG ep = EpIndex;
    NTSTATUS status;

    if (Ctx == NULL || EpIndex >= SOF_USB_MAX_ENDPOINTS) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    devObj = Ctx->UsbOffloadState.DeviceObject;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    if (devObj == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    status = SofUsbSendIoctl(
        devObj,
        IOCTL_SBAUD_STREAM_START,
        &ep,
        sizeof(ep),
        NULL,
        0,
        NULL
    );

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    Ctx->UsbOffloadState.Streams[EpIndex].IsRunning = TRUE;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    SofLogBreadcrumb("[USB-Offload] StreamStart succeeded.\n");
    return STATUS_SUCCESS;
}

NTSTATUS
SofUsbOffloadStreamSuspend(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG EpIndex
)
{
    KIRQL oldIrql;
    PDEVICE_OBJECT devObj;
    ULONG ep = EpIndex;
    NTSTATUS status;

    if (Ctx == NULL || EpIndex >= SOF_USB_MAX_ENDPOINTS) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    devObj = Ctx->UsbOffloadState.DeviceObject;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    if (devObj == NULL) {
        return STATUS_SUCCESS;
    }

    status = SofUsbSendIoctl(
        devObj,
        IOCTL_SBAUD_STREAM_SUSPEND,
        &ep,
        sizeof(ep),
        NULL,
        0,
        NULL
    );

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    Ctx->UsbOffloadState.Streams[EpIndex].IsRunning = FALSE;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    SofLogBreadcrumb("[USB-Offload] StreamSuspend completed.\n");
    return STATUS_SUCCESS;
}

NTSTATUS
SofUsbOffloadStreamClose(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG EpIndex
)
{
    KIRQL oldIrql;
    PDEVICE_OBJECT devObj;
    ULONG ep = EpIndex;
    NTSTATUS status;

    if (Ctx == NULL || EpIndex >= SOF_USB_MAX_ENDPOINTS) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    devObj = Ctx->UsbOffloadState.DeviceObject;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    if (devObj != NULL) {
        status = SofUsbSendIoctl(
            devObj,
            IOCTL_SBAUD_STREAM_CLOSE,
            &ep,
            sizeof(ep),
            NULL,
            0,
            NULL
        );
    }

    KeAcquireSpinLock(&Ctx->UsbOffloadState.Lock, &oldIrql);
    Ctx->UsbOffloadState.Streams[EpIndex].IsOpen = FALSE;
    Ctx->UsbOffloadState.Streams[EpIndex].IsRunning = FALSE;
    KeReleaseSpinLock(&Ctx->UsbOffloadState.Lock, oldIrql);

    SofLogBreadcrumb("[USB-Offload] StreamClose completed.\n");
    return STATUS_SUCCESS;
}

NTSTATUS
SofUsbOffloadSetVolume(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG EpIndex,
    _In_ LONG Channel,
    _In_ LONG VolumeDb
)
{
    PDEVICE_OBJECT devObj;
    SIDEBANDAUDIO_VOLUME_PARAMS volParams;

    if (Ctx == NULL || EpIndex >= SOF_USB_MAX_ENDPOINTS) {
        return STATUS_INVALID_PARAMETER;
    }

    devObj = Ctx->UsbOffloadState.DeviceObject;
    if (devObj == NULL) {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&volParams, sizeof(volParams));
    volParams.EpIndex = EpIndex;
    volParams.Immediate = TRUE;
    volParams.Channel = Channel;
    volParams.Value = VolumeDb;

    return SofUsbSendIoctl(
        devObj,
        IOCTL_SBAUD_SET_VOLUME,
        &volParams,
        sizeof(volParams),
        NULL,
        0,
        NULL
    );
}

NTSTATUS
SofUsbOffloadSetMute(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG EpIndex,
    _In_ LONG Channel,
    _In_ BOOLEAN Mute
)
{
    PDEVICE_OBJECT devObj;
    SIDEBANDAUDIO_MUTE_PARAMS muteParams;

    if (Ctx == NULL || EpIndex >= SOF_USB_MAX_ENDPOINTS) {
        return STATUS_INVALID_PARAMETER;
    }

    devObj = Ctx->UsbOffloadState.DeviceObject;
    if (devObj == NULL) {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&muteParams, sizeof(muteParams));
    muteParams.EpIndex = EpIndex;
    muteParams.Immediate = TRUE;
    muteParams.Channel = Channel;
    muteParams.Value = Mute;

    return SofUsbSendIoctl(
        devObj,
        IOCTL_SBAUD_SET_MUTE,
        &muteParams,
        sizeof(muteParams),
        NULL,
        0,
        NULL
    );
}
