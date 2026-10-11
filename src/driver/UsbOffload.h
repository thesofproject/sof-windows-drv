/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file UsbOffload.h
 * @brief Sound Open Firmware (SOF) Windows Driver - USB Audio Offload Header.
 *
 * @details
 * Declares interfaces and data structures for interacting with the Microsoft Windows
 * Sideband Audio stack (`SidebandAudio.sys` / `UsbAudio2.sys`) to claim USB Audio Class (UAC)
 * devices, suppress default legacy USB audio streaming, and redirect audio data paths
 * through Intel DSP hardware pipelines.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_USB_OFFLOAD_H__
#define __SOF_USB_OFFLOAD_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#include <windows.h>
#endif

/**
 * @def SOF_USB_OFFLOAD_TAG
 * @brief Pool allocation tag for USB Offload objects ('Soub').
 */
#define SOF_USB_OFFLOAD_TAG         'buoS'

/**
 * @def SOF_USB_MAX_ENDPOINTS
 * @brief Maximum number of simultaneous USB audio endpoints supported.
 */
#define SOF_USB_MAX_ENDPOINTS       4

/**
 * @def SOF_USB_MAX_FORMATS
 * @brief Maximum number of supported audio formats cached per endpoint.
 */
#define SOF_USB_MAX_FORMATS         16

#define SOF_USB_DATAFLOW_IN         1   /**< Capture / Microphone dataflow */
#define SOF_USB_DATAFLOW_OUT        2   /**< Render / Speaker dataflow */

/* Sideband Audio IOCTL codes */
#define SIDEBANDAUDIO_IOCTL(_index_) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, _index_, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_SBAUD_GET_DEVICE_DESCRIPTOR                   SIDEBANDAUDIO_IOCTL(1)  /**< Query device endpoint count */
#define IOCTL_SBAUD_GET_ERROR_STATUS_UPDATE                 SIDEBANDAUDIO_IOCTL(2)  /**< Query sideband error status */
#define IOCTL_SBAUD_GET_ENDPOINT_DESCRIPTOR                 SIDEBANDAUDIO_IOCTL(3)  /**< Query endpoint characteristics */
#define IOCTL_SBAUD_GET_CONNECTION_STATUS_UPDATE            SIDEBANDAUDIO_IOCTL(4)  /**< Query jack connection status */
#define IOCTL_SBAUD_GET_VOLUMEPROPERTYVALUES                SIDEBANDAUDIO_IOCTL(5)  /**< Query volume range values */
#define IOCTL_SBAUD_SET_VOLUME                              SIDEBANDAUDIO_IOCTL(6)  /**< Set endpoint volume level */
#define IOCTL_SBAUD_GET_VOLUME_STATUS_UPDATE                SIDEBANDAUDIO_IOCTL(7)  /**< Asynchronous volume change update */
#define IOCTL_SBAUD_GET_MUTEPROPERTYVALUES                  SIDEBANDAUDIO_IOCTL(8)  /**< Query mute property capabilities */
#define IOCTL_SBAUD_SET_MUTE                                SIDEBANDAUDIO_IOCTL(9)  /**< Set endpoint mute state */
#define IOCTL_SBAUD_GET_MUTE_STATUS_UPDATE                  SIDEBANDAUDIO_IOCTL(10) /**< Asynchronous mute change update */
#define IOCTL_SBAUD_STREAM_OPEN                             SIDEBANDAUDIO_IOCTL(14) /**< Open sideband audio stream */
#define IOCTL_SBAUD_STREAM_CLOSE                            SIDEBANDAUDIO_IOCTL(15) /**< Close sideband audio stream */
#define IOCTL_SBAUD_STREAM_START                            SIDEBANDAUDIO_IOCTL(16) /**< Start isochronous sideband stream */
#define IOCTL_SBAUD_STREAM_SUSPEND                          SIDEBANDAUDIO_IOCTL(17) /**< Suspend sideband stream */
#define IOCTL_SBAUD_GET_STREAM_STATUS_UPDATE                SIDEBANDAUDIO_IOCTL(18) /**< Asynchronous stream status update */
#define IOCTL_SBAUD_GET_SUPPORTED_FORMATS                   SIDEBANDAUDIO_IOCTL(19) /**< Query supported audio formats */
#define IOCTL_SBAUD_SET_DEVICE_CLAIMED                      SIDEBANDAUDIO_IOCTL(24) /**< Claim device from legacy stack */

/* Sideband Audio payload structures */

/**
 * @struct _SIDEBANDAUDIO_DEVICE_DESCRIPTOR
 * @brief Output payload for IOCTL_SBAUD_GET_DEVICE_DESCRIPTOR.
 */
typedef struct _SIDEBANDAUDIO_DEVICE_DESCRIPTOR {
    ULONG NumberOfEndpoints;    /**< Total number of sideband-capable audio endpoints */
} SIDEBANDAUDIO_DEVICE_DESCRIPTOR, *PSIDEBANDAUDIO_DEVICE_DESCRIPTOR;

/**
 * @struct _SIDEBANDAUDIO_VOLUME_PARAMS
 * @brief Input payload for IOCTL_SBAUD_SET_VOLUME.
 */
typedef struct _SIDEBANDAUDIO_VOLUME_PARAMS {
    ULONG EpIndex;              /**< Endpoint index (0..NumberOfEndpoints-1) */
    int   Immediate;            /**< 1 for immediate update; 0 for queued */
    LONG  Channel;              /**< Target audio channel */
    LONG  Value;                /**< Volume value in 1/65536 dB units */
} SIDEBANDAUDIO_VOLUME_PARAMS, *PSIDEBANDAUDIO_VOLUME_PARAMS;

/**
 * @struct _SIDEBANDAUDIO_MUTE_PARAMS
 * @brief Input payload for IOCTL_SBAUD_SET_MUTE.
 */
typedef struct _SIDEBANDAUDIO_MUTE_PARAMS {
    ULONG EpIndex;              /**< Endpoint index */
    int   Immediate;            /**< 1 for immediate update */
    LONG  Channel;              /**< Target channel */
    int   Value;                /**< 1 for Mute, 0 for Unmute */
} SIDEBANDAUDIO_MUTE_PARAMS, *PSIDEBANDAUDIO_MUTE_PARAMS;

/**
 * @union _SIDEBANDAUDIO_STREAM_OPEN_PARAMS
 * @brief Input payload for IOCTL_SBAUD_STREAM_OPEN.
 */
typedef union _SIDEBANDAUDIO_STREAM_OPEN_PARAMS {
    struct {
        ULONG EpIndex;          /**< Endpoint index */
        PVOID Format;           /**< Pointer to KSDATAFORMAT */
        ULONG SiopCount;        /**< Sideband isochronous packet count */
    };
    LONGLONG Alignment;
} SIDEBANDAUDIO_STREAM_OPEN_PARAMS, *PSIDEBANDAUDIO_STREAM_OPEN_PARAMS;

/**
 * @struct _SOF_USB_STREAM_STATE
 * @brief Runtime state tracking an individual USB offload stream.
 */
typedef struct _SOF_USB_STREAM_STATE {
    ULONG               EpIndex;        /**< Associated endpoint index */
    BOOLEAN             IsOpen;         /**< TRUE if stream is open */
    BOOLEAN             IsRunning;      /**< TRUE if stream is actively transferring data */
    ULONG               Direction;      /**< SOF_USB_DATAFLOW_IN or SOF_USB_DATAFLOW_OUT */
    ULONG               SampleRate;     /**< Configured sample rate in Hz */
    ULONG               Channels;       /**< Number of channels */
    ULONG               BitDepth;       /**< Bit depth */
    LONG                VolumeDb[2];    /**< Cached volume levels in 1/65536 dB units */
    BOOLEAN             Mute;           /**< Cached mute status */
} SOF_USB_STREAM_STATE, *PSOF_USB_STREAM_STATE;

/**
 * @struct _SOF_USB_OFFLOAD_STATE
 * @brief Master controller state tracking attached USB audio offload interfaces.
 */
typedef struct _SOF_USB_OFFLOAD_STATE {
    PVOID               PnpNotificationHandle;  /**< WDF device interface arrival notification registration */
    UNICODE_STRING      SymbolicLinkName;       /**< Device interface symbolic link of sideband audio stack */
    PFILE_OBJECT        FileObject;             /**< Kernel file object representing open handle to sideband stack */
    PDEVICE_OBJECT      DeviceObject;           /**< Target device object */
    BOOLEAN             DevicePresent;          /**< TRUE if compatible USB audio device is physically connected */
    BOOLEAN             DeviceClaimed;          /**< TRUE if successfully claimed via IOCTL_SBAUD_SET_DEVICE_CLAIMED */
    BOOLEAN             LegacySuppressed;       /**< TRUE if default usbaudio2.sys driver is suppressed */

    /* Device & Endpoint Characteristics */
    SIDEBANDAUDIO_DEVICE_DESCRIPTOR DeviceDesc; /**< Device descriptor */
    ULONG               EndpointCount;          /**< Discovered endpoint count */

    /* Active Streams */
    SOF_USB_STREAM_STATE Streams[SOF_USB_MAX_ENDPOINTS]; /**< Array of stream descriptors */

    /* Synchronization */
    KSPIN_LOCK          Lock;                   /**< Spinlock protecting state transitions */

} SOF_USB_OFFLOAD_STATE, *PSOF_USB_OFFLOAD_STATE;

/* Forward declaration */
struct _SOF_DEVICE_CONTEXT;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the USB Audio Offload subsystem and registers for Sideband Audio interface arrival.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS
SofUsbOffloadInit(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx
);

/**
 * @brief Releases USB Audio Offload resources and unregisters PNP notifications.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 */
void
SofUsbOffloadRelease(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx
);

/**
 * @brief Claims a newly arrived USB audio device via Sideband Audio IOCTL to route data through DSP.
 *
 * @param[in,out] Ctx          Pointer to SOF device context.
 * @param[in]     SymbolicLink Interface symbolic link name.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS
SofUsbOffloadClaimDevice(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ PUNICODE_STRING SymbolicLink
);

/**
 * @brief Unclaims the USB audio device during device removal or driver teardown.
 *
 * @param[in,out] Ctx Pointer to SOF device context.
 */
void
SofUsbOffloadUnclaimDevice(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx
);

/**
 * @brief Opens a sideband stream on the specified endpoint.
 *
 * @param[in] Ctx        Pointer to SOF device context.
 * @param[in] EpIndex    Target endpoint index.
 * @param[in] SampleRate Sample rate in Hz.
 * @param[in] Channels   Channel count.
 * @param[in] BitDepth   Bit depth.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUsbOffloadStreamOpen(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ ULONG EpIndex,
    _In_ ULONG SampleRate,
    _In_ ULONG Channels,
    _In_ ULONG BitDepth
);

/**
 * @brief Starts isochronous streaming on the specified endpoint.
 *
 * @param[in] Ctx     Pointer to SOF device context.
 * @param[in] EpIndex Target endpoint index.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUsbOffloadStreamStart(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ ULONG EpIndex
);

/**
 * @brief Suspends isochronous streaming on the specified endpoint.
 *
 * @param[in] Ctx     Pointer to SOF device context.
 * @param[in] EpIndex Target endpoint index.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUsbOffloadStreamSuspend(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ ULONG EpIndex
);

/**
 * @brief Closes the sideband audio stream on the specified endpoint.
 *
 * @param[in] Ctx     Pointer to SOF device context.
 * @param[in] EpIndex Target endpoint index.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUsbOffloadStreamClose(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ ULONG EpIndex
);

/**
 * @brief Updates hardware volume on the USB sideband audio endpoint.
 *
 * @param[in] Ctx      Pointer to SOF device context.
 * @param[in] EpIndex  Target endpoint index.
 * @param[in] Channel  Target channel.
 * @param[in] VolumeDb Volume level in 1/65536 dB units.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUsbOffloadSetVolume(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ ULONG EpIndex,
    _In_ LONG Channel,
    _In_ LONG VolumeDb
);

/**
 * @brief Updates hardware mute state on the USB sideband audio endpoint.
 *
 * @param[in] Ctx     Pointer to SOF device context.
 * @param[in] EpIndex Target endpoint index.
 * @param[in] Channel Target channel.
 * @param[in] Mute    TRUE to mute; FALSE to unmute.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS
SofUsbOffloadSetMute(
    _In_ struct _SOF_DEVICE_CONTEXT *Ctx,
    _In_ ULONG EpIndex,
    _In_ LONG Channel,
    _In_ BOOLEAN Mute
);

#ifdef __cplusplus
}
#endif

#endif /* __SOF_USB_OFFLOAD_H__ */
