/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file WaveRt.h
 * @brief PortCls WaveRT Audio Streaming Engine Header for Sound Open Firmware.
 *
 * @details
 * Declares data structures, stream descriptors, and API functions for managing
 * circular WaveRT streaming buffers, Intel HD-A host stream DMA engines, Processing
 * Pipe Control (PPCTL) and Link Control (PPLC), and hardware wallclock time.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_WAVERT_H__
#define __SOF_WAVERT_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <portcls.h>
#else
#include <windows.h>
#include <strsafe.h>
#define RtlStringCbPrintfA StringCbPrintfA
#include <ks.h>
#endif

#include <sof/sof_osal.h>
#include "Hw.h"
#include "Topo.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def SOF_WAVERT_MAX_EVENTS
 * @brief Maximum number of registered periodic notification event handles per stream.
 */
#define SOF_WAVERT_MAX_EVENTS           8

/**
 * @def SOF_WAVERT_DEFAULT_PERIOD_MS
 * @brief Default period notification interval in milliseconds (10ms = 100Hz audio pump).
 */
#define SOF_WAVERT_DEFAULT_PERIOD_MS    10

/* Stream Tag Allocation */
#define SOF_WAVERT_STREAM_TAG_PLAYBACK      1   /**< Default HDA Playback Stream Tag */
#define SOF_WAVERT_STREAM_TAG_CAPTURE       1   /**< Default HDA Capture Stream Tag */
#define SOF_WAVERT_STREAM_TAG_HDMI_1        2   /**< Display Audio Link 1 Stream Tag */
#define SOF_WAVERT_STREAM_TAG_HDMI_2        3   /**< Display Audio Link 2 Stream Tag */
#define SOF_WAVERT_STREAM_TAG_HDMI_3        4   /**< Display Audio Link 3 Stream Tag */
#define SOF_WAVERT_STREAM_TAG_DMIC          2   /**< Capture Stream Tag 2: PDM Digital Mic */
#define SOF_WAVERT_STREAM_TAG_SSP_RENDER    5   /**< Playback Stream Tag 5: I2S SSP2 Render */
#define SOF_WAVERT_STREAM_TAG_SSP_CAPTURE   3   /**< Capture Stream Tag 3: I2S SSP2 Capture */
#define SOF_WAVERT_STREAM_TAG_SDW_RENDER    6   /**< Playback Stream Tag 6: SoundWire Render */
#define SOF_WAVERT_STREAM_TAG_SDW_CAPTURE   4   /**< Capture Stream Tag 4: SoundWire Capture */

/* SOF IPC4 Pipeline IDs */
#define SOF_PIPELINE_ID_PLAYBACK            1   /**< HDA Analog Render Pipeline ID */
#define SOF_PIPELINE_ID_CAPTURE             2   /**< HDA Analog Capture Pipeline ID */
#define SOF_PIPELINE_ID_HDMI_1              3   /**< Intel Display Audio Pipe 1 ID */
#define SOF_PIPELINE_ID_HDMI_2              4   /**< Intel Display Audio Pipe 2 ID */
#define SOF_PIPELINE_ID_HDMI_3              5   /**< Intel Display Audio Pipe 3 ID */
#define SOF_PIPELINE_ID_DMIC                6   /**< PDM Digital Mic Pipeline ID */
#define SOF_PIPELINE_ID_SSP_PLAYBACK        7   /**< I2S SSP Playback Pipeline ID */
#define SOF_PIPELINE_ID_SSP_CAPTURE         8   /**< I2S SSP Capture Pipeline ID */
#define SOF_PIPELINE_ID_SDW_PLAYBACK        9   /**< SoundWire Playback Pipeline ID */
#define SOF_PIPELINE_ID_SDW_CAPTURE         10  /**< SoundWire Capture Pipeline ID */

/**
 * @def SOF_HDA_STREAM_REG_OFFSET
 * @brief Base MMIO offset for an HD-A stream descriptor on BAR0 (0x80 + (index * 0x20)).
 */
#define SOF_HDA_STREAM_REG_OFFSET(idx)  (0x80 + ((idx) * 0x20))

/**
 * @def SOF_HDA_REG_WALCLK
 * @brief 24 MHz HD-A Wall Clock counter register offset on BAR0.
 */
#define SOF_HDA_REG_WALCLK              0x30

/* Forward declarations */
typedef struct _SOF_DEVICE_CONTEXT *PSOF_DEVICE_CONTEXT;

/**
 * @struct _SOF_WAVERT_STREAM
 * @brief Runtime state and hardware bindings of an active WaveRT audio stream instance.
 */
typedef struct _SOF_WAVERT_STREAM {
    /** @brief Flag indicating stream has been created and initialized. */
    BOOLEAN             IsActive;

    /** @brief TRUE if stream is capture/recording; FALSE if render/playback. */
    BOOLEAN             IsCapture;

    /** @brief Target hardware audio endpoint (HDA, HDMI, DMIC, SSP, SDW, USB). */
    SOF_ENDPOINT_TYPE   EndpointType;

    /** @brief Hardware HD-A Stream Tag programmed in SD_CTL (1..15). */
    ULONG               StreamTag;

    /** @brief Hardware stream descriptor index on BAR0 (0..NumStreams-1). */
    ULONG               StreamIndex;

    /** @brief Associated SOF IPC4 Pipeline ID in DSP firmware. */
    ULONG               PipelineId;

    /** @brief Current Kernel Streaming state (KSSTATE_STOP, PAUSE, RUN). */
    KSSTATE             State;

    /** @brief Allocated IPC4 Host Copier module instance ID. */
    ULONG               HostCopierInstance;

    /** @brief Allocated IPC4 Gain / Volume module instance ID. */
    ULONG               GainInstance;

    /** @brief Allocated IPC4 DAI Copier module instance ID. */
    ULONG               DaiCopierInstance;

    /* Audio Format */
    /** @brief PCM audio format parameters (sample rate, bit depth, channel map). */
    SOF_AUDIO_FORMAT_PARAMS Format;

    /* Cyclic Buffer Allocation */
    /** @brief Total size of circular ring buffer in bytes. */
    ULONG               BufferSizeBytes;

    /** @brief Number of bytes transferred per period notification. */
    ULONG               PeriodSizeBytes;

    /** @brief Period notification interval in milliseconds. */
    ULONG               PeriodMs;

    /** @brief Kernel virtual address of circular DMA buffer. */
    PVOID               KernelBuffer;

    /** @brief Physical base address of circular DMA buffer. */
    PHYSICAL_ADDRESS    PhysicalAddress;

#ifdef _KERNEL_MODE
    /** @brief Memory Descriptor List (MDL) for user-mode mapping into audiosrv. */
    PMDL                Mdl;

    /** @brief User-mode virtual mapping address in Windows Audio Service. */
    PVOID               UserMapping;

    /** @brief DPC worker for periodic notification timer callbacks. */
    KDPC                NotificationDpc;

    /** @brief High-resolution kernel timer for period notifications. */
    KTIMER              NotificationTimer;

    /** @brief Registered notification event objects signaled on period boundaries. */
    PKEVENT             NotificationEvents[SOF_WAVERT_MAX_EVENTS];
#else
    PVOID               NotificationEvents[SOF_WAVERT_MAX_EVENTS];
#endif
    /** @brief Number of registered notification events in NotificationEvents. */
    ULONG               NumNotificationEvents;

    /** @brief Monotonically advancing linear byte counter for position fallback. */
    ULONG               LinearPositionBytes;

    /* Buffer Descriptor List (BDL) */
    /** @brief Pointer to allocated Buffer Descriptor List memory. */
    PSOF_INTEL_DSP_BDL  Bdl;

    /** @brief Physical base address of BDL table. */
    PHYSICAL_ADDRESS    BdlPhysicalAddress;

    /* Volume and Audio Controls */
    /** @brief Channel 0 and 1 volume in 1/65536 dB units (0 = 0 dB, -96dB min). */
    LONG                VolumeLevelDb[2];

    /** @brief Per-channel mute states. */
    BOOLEAN             IsMuted[2];

    /** @brief Master mute status. */
    BOOLEAN             MasterMute;

    /* Associated Device Context */
    /** @brief Pointer to parent SOF device context. */
    PSOF_DEVICE_CONTEXT DeviceContext;

} SOF_WAVERT_STREAM, *PSOF_WAVERT_STREAM;

/* Stream Lifecycle Management */

/**
 * @brief Allocates and initializes a WaveRT stream instance.
 *
 * Allocates circular buffer, builds 2-entry double-buffered BDL table, creates
 * the corresponding SOF IPC4 audio pipeline in DSP firmware, and programs
 * host HD-A stream registers.
 *
 * @param[in]  Ctx             Pointer to SOF device context.
 * @param[in]  EndpointType    Target audio endpoint type.
 * @param[in]  IsCapture       TRUE for recording; FALSE for playback.
 * @param[in]  StreamTag       Preferred HD-A stream tag (1..15); 0 for default.
 * @param[in]  Format          Audio format specification.
 * @param[in]  BufferSizeBytes Requested cyclic buffer size in bytes.
 * @param[in]  PeriodMs        Period notification duration in milliseconds.
 * @param[out] StreamOut       Receives allocated SOF_WAVERT_STREAM pointer.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
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
);

/**
 * @brief Frees a WaveRT stream instance and cleans up allocated resources.
 *
 * Stops streaming engines, unbinds and deletes the DSP IPC4 pipeline, cancels
 * timers, and frees contiguous physical memory allocations.
 *
 * @param[in] Stream Pointer to stream instance to destroy.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtFreeStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/* State Control */

/**
 * @brief Transitions a WaveRT stream between Kernel Streaming states (STOP, ACQUIRE, PAUSE, RUN).
 *
 * Starts or stops HD-A DMA engines, Link DMA (PPLC), codec converters, DSP pipelines,
 * and periodic notification timers.
 *
 * @param[in] Stream   Pointer to stream instance.
 * @param[in] NewState Target KSSTATE.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofWaveRtSetState(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ KSSTATE NewState
);

/* Position and Wallclock */

/**
 * @brief Queries current stream byte position within the circular buffer.
 *
 * @param[in]  Stream        Pointer to stream instance.
 * @param[out] PositionBytes Receives byte offset (0..BufferSizeBytes-1).
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtGetPosition(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PULONG PositionBytes
);

/**
 * @brief Reads the hardware wall clock timestamp converted to nanoseconds.
 *
 * @param[in]  Stream           Pointer to stream instance.
 * @param[out] ClockNanoseconds Receives wall clock time in nanoseconds.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtGetClockTime(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PULONGLONG ClockNanoseconds
);

/* Event Notification */

/**
 * @brief Registers a client notification event object to be signaled on period boundaries.
 *
 * @param[in] Stream            Pointer to stream instance.
 * @param[in] NotificationEvent Pointer to KEVENT to register.
 * @return STATUS_SUCCESS on success; STATUS_INSUFFICIENT_RESOURCES if maximum reached.
 */
NTSTATUS SofWaveRtRegisterNotificationEvent(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ PVOID NotificationEvent
);

/**
 * @brief Unregisters a previously registered periodic notification event.
 *
 * @param[in] Stream            Pointer to stream instance.
 * @param[in] NotificationEvent Pointer to KEVENT to unregister.
 * @return STATUS_SUCCESS on success; STATUS_NOT_FOUND if not present.
 */
NTSTATUS SofWaveRtUnregisterNotificationEvent(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ PVOID NotificationEvent
);

/**
 * @brief Signals all registered periodic notification events for a stream.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtSignalNotification(
    _In_ PSOF_WAVERT_STREAM Stream
);

/* BAR0 HDA Stream Engine Programming */

/**
 * @brief Programs HD-A stream registers on BAR0 for host DMA operation.
 *
 * Sets up stream reset, decoupling via PPCTL, BDL physical address, cyclic buffer length,
 * format word, and stream tag.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtConfigureHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/**
 * @brief Starts the host HD-A stream DMA engine by asserting SD_CTL.DMA_START.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStartHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/**
 * @brief Stops the host HD-A stream DMA engine by clearing SD_CTL.DMA_START.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStopHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/**
 * @brief Resets the host HD-A stream DMA engine (asserts then clears SD_CTL.SRST).
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtResetHdaStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/* Link Stream Engine Programming (PPLC) */

/**
 * @brief Programs Processing Pipe Link Control (PPLC) registers for DSP-to-codec link DMA.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtConfigureLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/**
 * @brief Starts the Link DMA engine by setting SOF_HDA_PPLCCTL_RUN.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStartLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/**
 * @brief Stops the Link DMA engine by clearing SOF_HDA_PPLCCTL_RUN.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStopLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/**
 * @brief Resets the Link DMA engine via PPLCCTL.STRST.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtResetLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
);

/* Audio Control Operations */

/**
 * @brief Sets channel volume in 1/65536 dB units and dispatches IPC4 message to DSP Gain module.
 *
 * @param[in] Stream   Pointer to stream instance.
 * @param[in] Channel  Channel index (0, 1, or -1 for master).
 * @param[in] VolumeDb Volume in 1/65536 dB units (clamped to [-96 dB, 0 dB]).
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtSetVolume(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _In_ LONG VolumeDb
);

/**
 * @brief Queries current channel volume setting.
 *
 * @param[in]  Stream      Pointer to stream instance.
 * @param[in]  Channel     Channel index (0 or 1).
 * @param[out] VolumeDbOut Receives volume in 1/65536 dB units.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtGetVolume(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _Out_ PLONG VolumeDbOut
);

/**
 * @brief Sets master/channel mute status and dispatches IPC4 mute command to DSP Gain module.
 *
 * @param[in] Stream Pointer to stream instance.
 * @param[in] Mute   TRUE to mute; FALSE to unmute.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtSetMute(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ BOOLEAN Mute
);

/**
 * @brief Queries current mute status.
 *
 * @param[in]  Stream  Pointer to stream instance.
 * @param[out] MuteOut Receives TRUE if muted, FALSE otherwise.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtGetMute(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PBOOLEAN MuteOut
);

/**
 * @brief Queries peak audio meter level for volume UI visualization.
 *
 * @param[in]  Stream           Pointer to stream instance.
 * @param[in]  Channel          Channel index (0 or 1).
 * @param[out] PeakMeterDbOut   Receives peak level in 1/65536 dB units.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtGetPeakMeter(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _Out_ PLONG PeakMeterDbOut
);

#ifdef __cplusplus
}
#endif

#endif /* __SOF_WAVERT_H__ */
