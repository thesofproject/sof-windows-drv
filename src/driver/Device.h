/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Device.h
 * @brief Device Context, Hardware Resource Bindings, and PnP/Power Definitions for SOF Windows Driver.
 *
 * Defines the main device context structure (SOF_DEVICE_CONTEXT), DMA stream engine descriptors,
 * firmware boot state transitions, and PnP/Power event callback prototypes.
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#ifndef __SOF_DEVICE_H__
#define __SOF_DEVICE_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <hdaudio.h>
#include "UsbOffload.h"
#else
#include <windows.h>
typedef UCHAR KIRQL;
typedef ULONG_PTR KAFFINITY;
typedef ULONG_PTR KSPIN_LOCK;
typedef PVOID KEVENT;
typedef PVOID PKEVENT;
typedef PVOID PMDL;
typedef PVOID PDEVICE_OBJECT;
typedef PVOID WDFTIMER;
typedef ULONG DEVICE_POWER_STATE;
typedef PVOID WDFDEVICE;
typedef PVOID WDFINTERRUPT;
typedef PVOID WDFDMAENABLER;
typedef struct _HDAUDIO_BUS_INTERFACE_V2 { int dummy; } HDAUDIO_BUS_INTERFACE_V2;
typedef struct _HDAUDIO_BUS_INTERFACE { int dummy; } HDAUDIO_BUS_INTERFACE;
typedef struct _HDAUDIO_CONVERTER_FORMAT { USHORT ConverterFormat; } HDAUDIO_CONVERTER_FORMAT;
#endif
#include <sof/sof_osal.h>
#include "PciIds.h"
#include "Hw.h"
#include "Ipc.h"
#include "Nhlt.h"
#include "Topo.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Appends a diagnostic breadcrumb log entry to C:\sof_entry.txt.
 * @param[in] text Null-terminated ASCII string to log.
 */
void SofLogBreadcrumb(const char* text);
#ifdef __cplusplus
}
#endif

/**
 * @brief Diagnostic assertion macro: logs failure to breadcrumb file and returns status code.
 */
#define SOF_VALIDATE_OR_BAIL(cond, status, msg) \
    do { \
        if (!(cond)) { \
            char _err[128]; \
            RtlStringCbPrintfA(_err, sizeof(_err), "[ASSERT-FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            SofLogBreadcrumb(_err); \
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", _err); \
            return (status); \
        } \
    } while (0)

/** @brief Memory allocation pool tag for SOF driver objects ('WSOF' / 0x464F5357). */
#define SOF_DEVICE_TAG 'FOSW'

/* Memory BAR Indices */

/** @brief PCI Base Address Register 0 index: HD Audio Controller registers. */
#define SOF_BAR_HDA_INDEX               0

/** @brief PCI Base Address Register 4 index: cAVS / ACE DSP Shim, SRAM, and Mailboxes. */
#define SOF_BAR_DSP_INDEX               4

/**
 * @enum _SOF_FW_BOOT_STATE
 * @brief Tracks the initialization and execution lifecycle of the DSP firmware binary.
 */
typedef enum _SOF_FW_BOOT_STATE {
    /** @brief Initial state before ROM loader setup or firmware download. */
    SOF_FW_BOOT_NOT_STARTED = 0,
    /** @brief Host DMA stream is streaming binary into DSP memory, or ROM is executing. */
    SOF_FW_BOOT_IN_PROGRESS,
    /** @brief Firmware boot handshake timed out or ROM reported error code in FSR. */
    SOF_FW_BOOT_FAILED,
    /** @brief DSP firmware sent FW_READY notification; IPC4 message pump operational. */
    SOF_FW_BOOT_COMPLETE
} SOF_FW_BOOT_STATE;

/** @brief Maximum number of legacy HDA link DMA stream engines supported. */
#define SOF_HDA_MAX_ENGINES             4

/* Forward declaration */
struct _SOF_DEVICE_CONTEXT;

/**
 * @struct _SOF_HDA_DMA_ENGINE
 * @brief Represents an Intel HDA Stream Descriptor DMA engine in BAR0.
 *
 * Used for firmware code loading (Stream 0), Link DMA playback/capture,
 * and USB Audio Offload (UAOL) gateway streaming.
 */
typedef struct _SOF_HDA_DMA_ENGINE {
    /** @brief Physical stream descriptor index in BAR0 (0..15). */
    ULONG                       StreamIndex;

    /** @brief Stream tag identifying packet headers on HDA/SDW link (1..15). */
    UCHAR                       StreamTag;

    /** @brief TRUE for input/capture stream; FALSE for output/playback. */
    BOOLEAN                     IsCapture;

    /** @brief TRUE if stream is currently assigned to an active pipeline or transfer. */
    BOOLEAN                     InUse;

    /** @brief Back-pointer to parent device extension context. */
    struct _SOF_DEVICE_CONTEXT  *Ctx;

    /** @brief Non-paged virtual address of cyclic DMA ring buffer. */
    PVOID                       BufferVa;

    /** @brief Physical 64-bit base address of cyclic DMA ring buffer. */
    PHYSICAL_ADDRESS            BufferPa;

    /** @brief Size in bytes of cyclic DMA ring buffer. */
    SIZE_T                      BufferSize;

    /** @brief Memory Descriptor List (MDL) locking cyclic buffer into physical RAM. */
    PMDL                        BufferMdl;

    /** @brief Non-paged virtual address of 128-bit aligned BDL descriptor list. */
    PVOID                       BdlVa;

    /** @brief Physical 64-bit address of BDL descriptor list written to SD_BDLPL/U. */
    PHYSICAL_ADDRESS            BdlPa;

    /** @brief Synchronization event signaled on DMA period completion. */
    PKEVENT                     NotificationEvent;

    /** @brief Converter format word programmed into SD_FORMAT. */
    HDAUDIO_CONVERTER_FORMAT    ConverterFormat;
} SOF_HDA_DMA_ENGINE, *PSOF_HDA_DMA_ENGINE;

/**
 * @struct _SOF_DEVICE_CONTEXT
 * @brief Primary per-device context structure for each SOF Audio DSP instance.
 *
 * Associated with the WDFDEVICE object. Contains mapped MMIO virtual addresses,
 * WDF object handles, interrupt synchronization, firmware state machines,
 * PortCls audio interfaces, and hardware subsystem components (SDW, UAOL, Display).
 */
typedef struct _SOF_DEVICE_CONTEXT {
    /** @brief WDF framework device object handle. */
    WDFDEVICE           WdfDevice;

    /** @brief WDF framework interrupt object handle. */
    WDFINTERRUPT        WdfInterrupt;

    /** @brief WDF framework DMA enabler handle for scatter/gather mapping. */
    WDFDMAENABLER       DmaEnabler;

    /* PCI Identity */
    /** @brief PCI Vendor ID (0x8086 for Intel, 0x1022 for AMD). */
    USHORT              VendorId;
    /** @brief PCI Device ID (e.g. 0xE428 for Panther Lake). */
    USHORT              DeviceId;
    /** @brief PCI Stepping / Revision ID. */
    UCHAR               RevisionId;

    /** @brief Pointer to platform hardware descriptor. */
    PSOF_DSP_HW_DESC    HwDesc;

    /* BAR0: HD Audio Controller (Stream DMA engines) */
    /** @brief Physical bus address of BAR0. */
    PHYSICAL_ADDRESS    Bar0PhysicalAddress;
    /** @brief Length in bytes of BAR0 MMIO space. */
    ULONG               Bar0Length;
    /** @brief Kernel virtual address mapping BAR0 registers. */
    PVOID               Bar0VirtualAddress;
    /** @brief Number of capture streams reported in GCAP. */
    ULONG               NumCaptureStreams;
    /** @brief Number of playback streams reported in GCAP. */
    ULONG               NumPlaybackStreams;
    /** @brief Offset in BAR0 to Processing Pipe (PP) Capability (0x3). */
    ULONG               PpCapOffset;
    /** @brief Offset in BAR0 to Software Position-in-Buffer (SPIB) Capability (0x4). */
    ULONG               SpbCapOffset;
    /** @brief Offset in BAR0 to Multiple Links (ML) Capability (0x2). */
    ULONG               MlCapOffset;

    /* BAR4: DSP Shim, Mailbox, and SRAM */
    /** @brief Physical bus address of BAR4. */
    PHYSICAL_ADDRESS    Bar4PhysicalAddress;
    /** @brief Length in bytes of BAR4 MMIO space. */
    ULONG               Bar4Length;
    /** @brief Kernel virtual address mapping BAR4 DSP registers and mailboxes. */
    PVOID               Bar4VirtualAddress;

    /* Interrupt Resource */
    /** @brief Assigned interrupt vector. */
    ULONG               InterruptVector;
    /** @brief Assigned interrupt IRQL. */
    KIRQL               InterruptIrql;
    /** @brief Processor affinity mask for interrupt delivery. */
    KAFFINITY           InterruptAffinity;
    /** @brief TRUE if interrupt is Message Signaled (MSI/MSI-X); FALSE for Line. */
    BOOLEAN             IsMsi;

    /* IPC Synchronization and State */
    /** @brief Spinlock serializing IPC message transmissions. */
    KSPIN_LOCK          IpcLock;
    /** @brief Event signaled when DSP acknowledges an IPC message (HIPCIDA). */
    KEVENT              IpcReplyEvent;
    /** @brief Event signaled when DSP sends FW_READY notification upon boot. */
    KEVENT              FwReadyEvent;
    /** @brief TRUE if host currently has an in-flight command pending completion. */
    BOOLEAN             IpcTxBusy;
    /** @brief Cached reply header structure from completed transaction. */
    SOF_IPC_MESSAGE     PendingReply;
    /** @brief Buffer receiving large payload data from DSP mailbox. */
    PVOID               ReplyBuffer;
    /** @brief Size in bytes of ReplyBuffer. */
    ULONG               ReplyBufferSize;
    /** @brief Current lifecycle state of DSP firmware execution. */
    SOF_FW_BOOT_STATE   FwState;

    /* State Tracking */
    /** @brief TRUE if EvtDevicePrepareHardware has successfully executed. */
    BOOLEAN             HardwarePrepared;
    /** @brief Current power state (PowerDeviceD0, PowerDeviceD3, etc.). */
    DEVICE_POWER_STATE  CurrentPowerState;
    /** @brief TRUE if an on-board HDA codec was detected on the link. */
    BOOLEAN             HdaCodecPresent;
    /** @brief TRUE if Intel Graphics Display Audio codec was detected. */
    BOOLEAN             IDispCodecPresent;
    /** @brief PCI Vendor ID of Intel Graphics display codec. */
    ULONG               GfxCodecVendorId;
    /** @brief PCI Revision ID of Intel Graphics display codec. */
    ULONG               GfxCodecRevisionId;
    /** @brief Active HDMI/DP display pin widget index. */
    ULONG               ActiveDisplayPin;
    /** @brief Audio Function Group (AFG) Node ID of display codec. */
    ULONG               DisplayAfgNid;
    /** @brief Array of Display Audio Converter Node IDs. */
    ULONG               DisplayCvtNid[4];
    /** @brief Number of valid display converter nodes found. */
    ULONG               DisplayCvtCount;
    /** @brief Array of Display Audio Pin Complex Node IDs. */
    ULONG               DisplayPinNid[4];
    /** @brief Number of valid display pin complex nodes found. */
    ULONG               DisplayPinCount;
    /** @brief Connection status array for up to 3 HDMI/DP displays. */
    BOOLEAN             DisplayConnected[3];
    /** @brief Friendly monitor names extracted from EDID/ELD blocks. */
    WCHAR               DisplayMonitorName[3][32];
    /** @brief Bitmask of supported sample rates (32k to 192k) per display. */
    ULONG               DisplaySampleRates[3];
    /** @brief Bitmask of supported bit depths (16-bit to 24-bit) per display. */
    ULONG               DisplayBitDepths[3];
    /** @brief Maximum audio channels supported by connected display sink. */
    ULONG               DisplayMaxChannels[3];

    /* Module Instance ID Bitmaps */
    /** @brief Allocation tracking bitmap for IPC4 copier module instances. */
    ULONG               CopierInstanceMask;
    /** @brief Allocation tracking bitmap for IPC4 gain/volume module instances. */
    ULONG               GainInstanceMask;

    /* Linux SOF compatibility device structure */
    /** @brief Embedded Linux device structure for shared core SOF functions. */
    struct device       LinuxDev;

    /* WaveRT Audio Streams */
    /** @brief Active speakers playback stream object pointer. */
    PVOID               PlaybackStream;
    /** @brief Active microphone capture stream object pointer. */
    PVOID               CaptureStream;
    /** @brief Active HDMI/DP audio streams array. */
    PVOID               HdmiStreams[3];
    /** @brief Active digital microphone (DMIC) stream pointer. */
    PVOID               DmicStream;
    /** @brief Active I2S/SSP playback stream pointer. */
    PVOID               SspPlaybackStream;
    /** @brief Active I2S/SSP capture stream pointer. */
    PVOID               SspCaptureStream;

    /* ACPI NHLT Topology Info */
    /** @brief Parsed ACPI Non-HDA Link Table (NHLT) endpoint descriptor cache. */
    SOF_NHLT_INFO       NhltInfo;

    /* Cached Audio Control State (persists across stream sessions) */
    /** @brief Cached volume levels in decibels per endpoint and channel. */
    LONG                DefaultVolumeLevelDb[SOF_ENDPOINT_COUNT][2];
    /** @brief Cached mute flags per endpoint. */
    BOOLEAN             DefaultMute[SOF_ENDPOINT_COUNT];

    /* PortCls WaveRT Subdevice State */
    /** @brief IPortWaveRT COM interface pointer. */
    PVOID               WavePort;
    /** @brief PortCls audio functional device object pointer. */
    PDEVICE_OBJECT      AudioPortClsDevice;
    /** @brief TRUE if PortCls subdevices have been successfully registered. */
    BOOLEAN             PortClsRegistered;
    /** @brief IPortEvents COM interface pointer for jack notification events. */
    PVOID               PortEvents;
    /** @brief Periodic passive WDF timer for jack and display presence polling. */
    WDFTIMER            JackPollTimer;
    /** @brief Delayed passive WDF timer to confirm healthy boot and reset watchdog. */
    WDFTIMER            WatchdogHealthTimer;

    /* Safety & Testing Canary Guards (Registry Configurable) */
    /** @brief 1 if testing canary is armed; 0 for safe mode. */
    ULONG               ArmedTesting;
    /** @brief 1 if PortCls subdevices should be enabled; 0 to bypass. */
    ULONG               EnablePortCls;

    /* Runtime Power Management & S0-Idle (D0i3) */
    /** @brief TRUE if S0-Idle power gating is enabled. */
    BOOLEAN             S0IdleEnabled;
    /** @brief Configured idle timeout in milliseconds before entering D0i3 (default 5000ms). */
    ULONG               S0IdleTimeoutMs;

    /* HDA Bus & Codec Child State */
    /** @brief Child WDF device object for HDA codec bus. */
    WDFDEVICE                   CodecChildDevice;
    /** @brief Codec PCI Vendor ID read from root node. */
    ULONG                       CodecVendorId;
    /** @brief Codec PCI Revision ID read from root node. */
    ULONG                       CodecRevisionId;
    /** @brief Spinlock serializing HDA Immediate Command/Response transactions. */
    KSPIN_LOCK                  HdaVerbLock;
    /** @brief HDAudio Bus Interface Version 2 vtable. */
    HDAUDIO_BUS_INTERFACE_V2    HdaBusInterfaceV2;
    /** @brief HDAudio Bus Interface Version 1 vtable. */
    HDAUDIO_BUS_INTERFACE       HdaBusInterfaceV1;
    /** @brief Array of HDA stream engines allocated for streaming. */
    SOF_HDA_DMA_ENGINE          HdaEngines[SOF_HDA_MAX_ENGINES];

    /* SoundWire Bus & Child State */
    /** @brief Child WDF device handles for SoundWire slave devices. */
    WDFDEVICE                   SdwChildDevices[4];
    /** @brief Number of discovered SoundWire child devices. */
    ULONG                       SdwChildCount;
    /** @brief TRUE if SoundWire master controller IP has been initialized. */
    BOOLEAN                     SdwControllerInitialized;
    /** @brief Spinlock guarding SoundWire bus operations. */
    KSPIN_LOCK                  SdwLock;

    /* USB Audio Offload & Link DMA State */
    /** @brief 1 if USB Audio Offload (UAOL) is enabled; 0 otherwise. */
    ULONG                       EnableUsbOffload;
    /** @brief USB audio offload state machine and sideband device context. */
    SOF_USB_OFFLOAD_STATE       UsbOffloadState;
    /** @brief Dedicated Link DMA engine for USB playback (Stream Tag 3 / Index 4). */
    SOF_HDA_DMA_ENGINE          UsbPlaybackEngine;
    /** @brief Dedicated Link DMA engine for USB capture (Stream Tag 4 / Index 5). */
    SOF_HDA_DMA_ENGINE          UsbCaptureEngine;

    /* Zephyr FW Logging & SRAM Debug Window State */
    /** @brief Offset in BAR4 to DSP SRAM Debug Window (0x190000 on ACE, 0xC0000 on cAVS). */
    ULONG                       DebugWindowOffset;
    /** @brief Array of host read pointer offsets for DSP cores [0..3]. */
    ULONG                       FwLogHostReadPtr[4];
    /** @brief Array of DSP slot offsets for cores [0..3]. */
    ULONG                       FwLogSlotOffset[4];
    /** @brief TRUE if FW logging has been initialized. */
    BOOLEAN                     FwLogInitialized;

} SOF_DEVICE_CONTEXT, *PSOF_DEVICE_CONTEXT;

#ifdef _KERNEL_MODE
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SOF_DEVICE_CONTEXT, GetDeviceContext)

/** @brief Periodic timer callback polling display pin sense and notifying audio clients. */
EVT_WDF_TIMER SofEvtJackPollTimer;

/** @brief One-shot timer callback verifying 10s of stable D0 operation. */
EVT_WDF_TIMER SofEvtWatchdogHealthTimer;

/** @brief KMDF EvtDevicePrepareHardware callback mapping BARs and initializing resources. */
EVT_WDF_DEVICE_PREPARE_HARDWARE SofEvtDevicePrepareHardware;

/** @brief KMDF EvtDeviceReleaseHardware callback unmapping MMIO and releasing resources. */
EVT_WDF_DEVICE_RELEASE_HARDWARE SofEvtDeviceReleaseHardware;

/** @brief KMDF EvtDeviceD0Entry callback powering up hardware and arming audio streams. */
EVT_WDF_DEVICE_D0_ENTRY SofEvtDeviceD0Entry;

/** @brief KMDF EvtDeviceD0Exit callback transitioning hardware to quiescent low-power state. */
EVT_WDF_DEVICE_D0_EXIT SofEvtDeviceD0Exit;

/** @brief High-IRQL Interrupt Service Routine (ISR) identifying doorbell events. */
EVT_WDF_INTERRUPT_ISR SofEvtInterruptIsr;

/** @brief Deferred Procedure Call (DPC) processing IPC messages at DISPATCH_LEVEL. */
EVT_WDF_INTERRUPT_DPC SofEvtInterruptDpc;

/**
 * @brief Performs one-time hardware setup, codec discovery, and PortCls registration.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] Context Pointer to device extension context.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
NTSTATUS SofInitializeDevice(
    _In_ WDFDEVICE Device,
    _In_ PSOF_DEVICE_CONTEXT Context
);

/**
 * @struct _SOF_SDCA_FILTER_CONTEXT
 * @brief Context for SoundWire SDCA Audio Function Lower Filter driver.
 */
typedef struct _SOF_SDCA_FILTER_CONTEXT {
    WDFDEVICE   Device;
    WDFIOTARGET Target;
} SOF_SDCA_FILTER_CONTEXT, *PSOF_SDCA_FILTER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SOF_SDCA_FILTER_CONTEXT, GetSdcaFilterContext)

/**
 * @brief Checks if device is a SoundWire Sound Class Audio (SDCA) function device.
 * @param[in] DeviceInit Pointer to device initialization structure.
 * @return BOOLEAN TRUE if matching SDCA hardware ID, FALSE otherwise.
 */
BOOLEAN SofIsSdcaFunctionDevice(
    _In_ PWDFDEVICE_INIT DeviceInit
);

/**
 * @brief Attaches SOF driver as a lower filter on SDCA function driver stack.
 * @param[in] Driver Handle to WDFDRIVER.
 * @param[in,out] DeviceInit Pointer to device initialization structure.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
NTSTATUS SofSdcaFilterDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
);

/** @brief Intercepts IOCTL requests sent to SDCA function driver. */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SofSdcaFilterEvtIoDeviceControl;
#endif

#endif /* __SOF_DEVICE_H__ */
