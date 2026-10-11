/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file PortClsAdapter.cpp
 * @brief PortCls COM WaveRT Miniport Adapter Implementation for Sound Open Firmware.
 *
 * @details
 * Exposes standard Microsoft Windows PortCls (`portcls.sys`) COM interfaces:
 *  - IMiniportWaveRT: Manages miniport adapter lifecycle, property requests (Volume,
 *    Mute, PeakMeter, JackDescription, JackSinkInfo, CpuResources), and audio stream creation.
 *  - IMiniportWaveRTStreamNotification: Manages individual stream instances, cyclic buffer
 *    allocation, MDL mapping into Windows Audio Service (audiosrv), state transitions,
 *    and period notification events.
 *  - IPowerNotify: Handles audio device power state notifications (D0 <-> D3).
 *
 * Audio Endpoint Mappings:
 *  - Pin 0/1: Analog Speaker / Mic (HD-A codec)
 *  - Pin 4/6/8: Intel Display Audio HDMI 1, 2, 3
 *  - Pin 10: Digital Microphone (DMIC PDM)
 *  - Pin 12/14: I2S SSP2 Render / Capture
 *  - Pin 16/18: USB Audio Offload Render / Capture (UAOL)
 * Copyright 2026 The SOF Authors
 */

#ifdef _KERNEL_MODE

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <portcls.h>
#include <ksmedia.h>
#include <ntstrsafe.h>
#include "PortClsAdapter.h"
#include "WaveRt.h"
#include "Device.h"
#include "HdaVerb.h"
#include "UsbOffload.h"
#include "UaolGateway.h"

/* Placement and Pool Allocators for Kernel-Mode C++ */
void* __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag) {
    return ExAllocatePool2(flags, size, tag);
}

void __cdecl operator delete(void* p, POOL_FLAGS flags, ULONG tag) {
    UNREFERENCED_PARAMETER(flags);
    if (p) ExFreePoolWithTag(p, tag);
}

void __cdecl operator delete(void* p, ULONG tag) {
    if (p) ExFreePoolWithTag(p, tag);
}

void __cdecl operator delete(void* p, size_t size) {
    UNREFERENCED_PARAMETER(size);
    if (p) ExFreePool(p);
}

/* Forward declaration of filter descriptor defined after miniport classes */
extern const PCFILTER_DESCRIPTOR FilterDescriptor;

/* =========================================================================
 * CMiniportWaveRTStreamSof (Stream Instance)
 * ========================================================================= */

/**
 * @class CMiniportWaveRTStreamSof
 * @brief PortCls WaveRT stream notification COM object.
 *
 * Implements IMiniportWaveRTStreamNotification to provide the Windows Audio Engine
 * with low-latency access to the circular audio buffer, hardware position registers,
 * and high-resolution periodic notification events.
 */
class CMiniportWaveRTStreamSof : public IMiniportWaveRTStreamNotification {
private:
    LONG                m_RefCount;
    PSOF_DEVICE_CONTEXT m_DevCtx;
    PSOF_WAVERT_STREAM  m_Stream;
    BOOLEAN             m_IsCapture;
    ULONG               m_PinId;
    SOF_AUDIO_FORMAT_PARAMS m_Format;
    PVOID               m_CachedEvents[SOF_WAVERT_MAX_EVENTS];
    ULONG               m_CachedEventCount;
    BOOLEAN             m_PowerRefHeld;

public:
    CMiniportWaveRTStreamSof(PSOF_DEVICE_CONTEXT DevCtx, ULONG PinId, BOOLEAN IsCapture)
        : m_RefCount(1), m_DevCtx(DevCtx), m_Stream(NULL), m_IsCapture(IsCapture), m_PinId(PinId), m_CachedEventCount(0), m_PowerRefHeld(FALSE)
    {
        RtlZeroMemory(m_CachedEvents, sizeof(m_CachedEvents));
        SofIpc4BuildDefaultStereoFormat(48000, 32, &m_Format);
        if (m_DevCtx != NULL && m_DevCtx->WdfDevice != NULL && m_DevCtx->S0IdleEnabled) {
            NTSTATUS pStatus = WdfDeviceStopIdle(m_DevCtx->WdfDevice, TRUE);
            if (NT_SUCCESS(pStatus)) {
                m_PowerRefHeld = TRUE;
                SofLogBreadcrumb("[Power] Stream acquired power reference (D0 active)\n");
            } else {
                char pmsg[128];
                RtlStringCbPrintfA(pmsg, sizeof(pmsg), "[Power] WdfDeviceStopIdle failed: 0x%08X\n", pStatus);
                SofLogBreadcrumb(pmsg);
            }
        }
    }

    ~CMiniportWaveRTStreamSof() {
        if (m_PowerRefHeld && m_DevCtx != NULL && m_DevCtx->WdfDevice != NULL) {
            WdfDeviceResumeIdle(m_DevCtx->WdfDevice);
            m_PowerRefHeld = FALSE;
            SofLogBreadcrumb("[Power] Stream released power reference in destructor (Idle allowed)\n");
        }
        if (m_Stream != NULL) {
            SofWaveRtFreeStream(m_Stream);
            m_Stream = NULL;
        }
    }

    PSOF_WAVERT_STREAM GetStream() const { return m_Stream; }

    /* IUnknown */
    STDMETHODIMP QueryInterface(REFIID riid, PVOID* ppv) {
        if (ppv == NULL) return STATUS_INVALID_PARAMETER;

        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IMiniportWaveRTStream) ||
            IsEqualGUID(riid, IID_IMiniportWaveRTStreamNotification))
        {
            *ppv = static_cast<IMiniportWaveRTStreamNotification*>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *ppv = NULL;
        return STATUS_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_RefCount);
    }

    STDMETHODIMP_(ULONG) Release() {
        LONG count = InterlockedDecrement(&m_RefCount);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    /* IMiniportWaveRTStream */
    STDMETHODIMP SetFormat(PKSDATAFORMAT DataFormat) {
        if (DataFormat == NULL) return STATUS_INVALID_PARAMETER;
        if (IsEqualGUID(DataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO))
        {
            if (IsEqualGUID(DataFormat->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
                IsEqualGUID(DataFormat->SubFormat, GUID_NULL))
            {
                PKSDATAFORMAT_WAVEFORMATEX waveFormat = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
                m_Format.SamplingFrequency = waveFormat->WaveFormatEx.nSamplesPerSec;
                m_Format.ChannelsCount = waveFormat->WaveFormatEx.nChannels;
                m_Format.BitDepth = waveFormat->WaveFormatEx.wBitsPerSample;
                m_Format.ValidBitDepth = m_Format.BitDepth;

                if (waveFormat->WaveFormatEx.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    waveFormat->WaveFormatEx.cbSize >= 22) {
                    PWAVEFORMATEXTENSIBLE pExt = (PWAVEFORMATEXTENSIBLE)&waveFormat->WaveFormatEx;
                    if (pExt->Samples.wValidBitsPerSample > 0) {
                        m_Format.ValidBitDepth = pExt->Samples.wValidBitsPerSample;
                    }
                }

                if (m_Format.ChannelsCount == 1) {
                    m_Format.ChannelConfig = SOF_IPC4_CHANNEL_CONFIG_MONO;
                    m_Format.ChannelMap = SOF_IPC4_CH_MAP_MONO;
                } else if (m_Format.ChannelsCount == 4) {
                    m_Format.ChannelConfig = SOF_IPC4_CHANNEL_CONFIG_QUATRO;
                    m_Format.ChannelMap = SOF_IPC4_CH_MAP_QUAD;
                } else if (m_Format.ChannelsCount == 6) {
                    m_Format.ChannelConfig = SOF_IPC4_CHANNEL_CONFIG_5_POINT_1;
                    m_Format.ChannelMap = SOF_IPC4_CH_MAP_5_1;
                } else if (m_Format.ChannelsCount == 8) {
                    m_Format.ChannelConfig = SOF_IPC4_CHANNEL_CONFIG_7_POINT_1;
                    m_Format.ChannelMap = SOF_IPC4_CH_MAP_7_1;
                } else {
                    m_Format.ChannelConfig = SOF_IPC4_CHANNEL_CONFIG_STEREO;
                    m_Format.ChannelMap = SOF_IPC4_CH_MAP_STEREO;
                }
                m_Format.InterleavingStyle = SOF_IPC4_CHANNELS_INTERLEAVED;

                char msg[128];
                RtlStringCbPrintfA(msg, sizeof(msg),
                    "[MiniportStream] SetFormat: %uHz, %uch, %ubit (valid %ubit)\n",
                    m_Format.SamplingFrequency, m_Format.ChannelsCount,
                    m_Format.BitDepth, m_Format.ValidBitDepth);
                SofLogBreadcrumb(msg);
                return STATUS_SUCCESS;
            }
        }
        return STATUS_INVALID_PARAMETER;
    }

    STDMETHODIMP SetState(KSSTATE State) {
        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg), "[MiniportStream] SetState: state=%u, stream=%p, pin=%u\n", State, m_Stream, m_PinId);
        SofLogBreadcrumb(msg);

        /* USB Audio Offload: Link DMA and UAOL Pipeline Lifecycle */
        if (m_PinId == 16 || m_PinId == 18) {
            BOOLEAN isCapture = (m_PinId == 18);
            if (State == KSSTATE_RUN) {
                if (m_Stream != NULL && m_DevCtx != NULL) {
                    SofUaolProgramLinkDma(
                        m_DevCtx,
                        isCapture,
                        m_Format.SamplingFrequency,
                        m_Format.ChannelsCount,
                        m_Format.BitDepth,
                        m_Stream->PhysicalAddress,
                        m_Stream->BufferSizeBytes
                    );
                    SofUaolStartPipeline(m_DevCtx, isCapture);
                }
            } else if (State == KSSTATE_PAUSE || State == KSSTATE_STOP) {
                if (m_DevCtx != NULL) {
                    SofUaolStopPipeline(m_DevCtx, isCapture);
                }
            }
        }

        if (m_Stream != NULL) {
            return SofWaveRtSetState(m_Stream, State);
        }
        return STATUS_SUCCESS;
    }

    STDMETHODIMP GetPosition(PKSAUDIO_POSITION Position) {
        if (Position == NULL) return STATUS_INVALID_PARAMETER;
        ULONG bytePos = 0;
        if (m_Stream != NULL) {
            SofWaveRtGetPosition(m_Stream, &bytePos);
        }
        Position->PlayOffset = bytePos;
        Position->WriteOffset = bytePos;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP AllocateAudioBuffer(
        ULONG RequestedSize,
        PMDL* AudioBufferMdl,
        ULONG* ActualSize,
        ULONG* OffsetFromFirstPage,
        MEMORY_CACHING_TYPE* CacheType
    ) {
        return AllocateBufferWithNotification(
            2, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType
        );
    }

    STDMETHODIMP_(VOID) FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize) {
        FreeBufferWithNotification(AudioBufferMdl, BufferSize);
    }

    STDMETHODIMP_(VOID) GetHWLatency(PKSRTAUDIO_HWLATENCY hwLatency) {
        if (hwLatency != NULL) {
            hwLatency->FifoSize = 1024;
            hwLatency->ChipsetDelay = 250;
            hwLatency->CodecDelay = 250;
        }
    }

    STDMETHODIMP GetPositionRegister(PKSRTAUDIO_HWREGISTER Register) {
        UNREFERENCED_PARAMETER(Register);
        return STATUS_NOT_SUPPORTED;
    }

    STDMETHODIMP GetClockRegister(PKSRTAUDIO_HWREGISTER Register) {
        UNREFERENCED_PARAMETER(Register);
        return STATUS_NOT_SUPPORTED;
    }

    /* IMiniportWaveRTStreamNotification */
    STDMETHODIMP AllocateBufferWithNotification(
        ULONG NotificationCount,
        ULONG RequestedSize,
        PMDL* AudioBufferMdl,
        ULONG* ActualSize,
        ULONG* OffsetFromFirstPage,
        MEMORY_CACHING_TYPE* CacheType
    ) {
        char reqMsg[128];
        RtlStringCbPrintfA(reqMsg, sizeof(reqMsg),
            "[MiniportStream] AllocateBufferWithNotification: ReqSize=%u, NotifCount=%u, stream=%p\n",
            RequestedSize, NotificationCount, m_Stream);
        SofLogBreadcrumb(reqMsg);

        if (m_Stream != NULL) {
            *AudioBufferMdl = m_Stream->Mdl;
            *ActualSize = m_Stream->BufferSizeBytes;
            *OffsetFromFirstPage = 0;
            *CacheType = MmCached;
            return STATUS_SUCCESS;
        }

        NTSTATUS status;
        ULONG streamTag = m_IsCapture ? SOF_WAVERT_STREAM_TAG_CAPTURE : SOF_WAVERT_STREAM_TAG_PLAYBACK;
        ULONG bufferSize = (RequestedSize > 0) ? RequestedSize : 65536;
        SOF_ENDPOINT_TYPE epType = SOF_ENDPOINT_HDMI_1;

        if (m_PinId == 4) {
            epType = SOF_ENDPOINT_HDMI_1;
            streamTag = SOF_WAVERT_STREAM_TAG_HDMI_1;
        } else if (m_PinId == 6) {
            epType = SOF_ENDPOINT_HDMI_2;
            streamTag = SOF_WAVERT_STREAM_TAG_HDMI_2;
        } else if (m_PinId == 8) {
            epType = SOF_ENDPOINT_HDMI_3;
            streamTag = SOF_WAVERT_STREAM_TAG_HDMI_3;
        } else if (m_PinId == 10) {
            epType = SOF_ENDPOINT_DMIC_PDM;
            streamTag = SOF_WAVERT_STREAM_TAG_DMIC;
        } else if (m_PinId == 12) {
            epType = SOF_ENDPOINT_I2S_SSP2;
            streamTag = SOF_WAVERT_STREAM_TAG_SSP_RENDER;
        } else if (m_PinId == 14) {
            epType = SOF_ENDPOINT_I2S_SSP2;
            streamTag = SOF_WAVERT_STREAM_TAG_SSP_CAPTURE;
        } else if (m_PinId == 16) {
            epType = SOF_ENDPOINT_USB_OFFLOAD;
            streamTag = SOF_USB_PLAYBACK_STREAM_TAG;
            if (m_DevCtx != NULL) {
                SofUsbOffloadStreamOpen(m_DevCtx, 0, m_Format.SamplingFrequency, m_Format.ChannelsCount, m_Format.BitDepth);
            }
        } else if (m_PinId == 18) {
            epType = SOF_ENDPOINT_USB_OFFLOAD;
            streamTag = SOF_USB_CAPTURE_STREAM_TAG;
            if (m_DevCtx != NULL) {
                SofUsbOffloadStreamOpen(m_DevCtx, 1, m_Format.SamplingFrequency, m_Format.ChannelsCount, m_Format.BitDepth);
            }
        } else if (m_DevCtx != NULL && m_DevCtx->HdaCodecPresent) {
            epType = SOF_ENDPOINT_HDA_ANALOG;
        } else if (m_IsCapture) {
            epType = SOF_ENDPOINT_I2S_SSP2;
        } else {
            epType = SOF_ENDPOINT_HDMI_1;
        }

        status = SofWaveRtCreateStream(
            m_DevCtx, epType, m_IsCapture, streamTag, &m_Format, bufferSize, 10, &m_Stream
        );
        if (!NT_SUCCESS(status)) {
            char msg[128];
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[MiniportStream] SofWaveRtCreateStream FAILED: 0x%08X\n", status);
            SofLogBreadcrumb(msg);
            return status;
        }

        /* Build MDL for user-mode mapping */
        m_Stream->Mdl = IoAllocateMdl(m_Stream->KernelBuffer, m_Stream->BufferSizeBytes, FALSE, FALSE, NULL);
        if (m_Stream->Mdl == NULL) {
            SofWaveRtFreeStream(m_Stream);
            m_Stream = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(m_Stream->Mdl);

        *AudioBufferMdl = m_Stream->Mdl;
        *ActualSize = m_Stream->BufferSizeBytes;
        *OffsetFromFirstPage = 0;
        *CacheType = MmCached;

        /* Store active stream in device context */
        if (m_PinId == 4) {
            m_DevCtx->HdmiStreams[0] = m_Stream;
        } else if (m_PinId == 6) {
            m_DevCtx->HdmiStreams[1] = m_Stream;
        } else if (m_PinId == 8) {
            m_DevCtx->HdmiStreams[2] = m_Stream;
        } else if (m_PinId == 10) {
            m_DevCtx->DmicStream = m_Stream;
        } else if (m_PinId == 12) {
            m_DevCtx->SspPlaybackStream = m_Stream;
        } else if (m_PinId == 14) {
            m_DevCtx->SspCaptureStream = m_Stream;
        } else if (m_IsCapture) {
            m_DevCtx->CaptureStream = m_Stream;
        } else {
            m_DevCtx->PlaybackStream = m_Stream;
        }

        /* Replay cached notification events into newly created stream */
        for (ULONG i = 0; i < m_CachedEventCount; i++) {
            if (m_CachedEvents[i] != NULL) {
                SofWaveRtRegisterNotificationEvent(m_Stream, m_CachedEvents[i]);
            }
        }

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[MiniportStream] AllocateBufferWithNotification: pin=%u, size=%u, %u events replayed\n",
            m_PinId, m_Stream->BufferSizeBytes, m_CachedEventCount);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(VOID) FreeBufferWithNotification(PMDL AudioBufferMdl, ULONG BufferSize) {
        UNREFERENCED_PARAMETER(BufferSize);
        SofLogBreadcrumb("[MiniportStream] FreeBufferWithNotification called\n");
        if (m_PowerRefHeld && m_DevCtx != NULL && m_DevCtx->WdfDevice != NULL) {
            WdfDeviceResumeIdle(m_DevCtx->WdfDevice);
            m_PowerRefHeld = FALSE;
            SofLogBreadcrumb("[Power] Stream released power reference in FreeBuffer (Idle allowed)\n");
        }
        if (m_Stream != NULL) {
            if (AudioBufferMdl != NULL && m_Stream->Mdl == AudioBufferMdl) {
                IoFreeMdl(AudioBufferMdl);
                m_Stream->Mdl = NULL;
            }
            if (m_PinId == 4) {
                if (m_DevCtx != NULL && m_DevCtx->HdmiStreams[0] == m_Stream) {
                    m_DevCtx->HdmiStreams[0] = NULL;
                }
            } else if (m_PinId == 6) {
                if (m_DevCtx != NULL && m_DevCtx->HdmiStreams[1] == m_Stream) {
                    m_DevCtx->HdmiStreams[1] = NULL;
                }
            } else if (m_PinId == 8) {
                if (m_DevCtx != NULL && m_DevCtx->HdmiStreams[2] == m_Stream) {
                    m_DevCtx->HdmiStreams[2] = NULL;
                }
            } else if (m_PinId == 10) {
                if (m_DevCtx != NULL && m_DevCtx->DmicStream == m_Stream) {
                    m_DevCtx->DmicStream = NULL;
                }
            } else if (m_PinId == 12) {
                if (m_DevCtx != NULL && m_DevCtx->SspPlaybackStream == m_Stream) {
                    m_DevCtx->SspPlaybackStream = NULL;
                }
            } else if (m_PinId == 14) {
                if (m_DevCtx != NULL && m_DevCtx->SspCaptureStream == m_Stream) {
                    m_DevCtx->SspCaptureStream = NULL;
                }
            } else if (m_IsCapture) {
                if (m_DevCtx != NULL && m_DevCtx->CaptureStream == m_Stream) {
                    m_DevCtx->CaptureStream = NULL;
                }
            } else {
                if (m_DevCtx != NULL && m_DevCtx->PlaybackStream == m_Stream) {
                    m_DevCtx->PlaybackStream = NULL;
                }
            }
            SofWaveRtFreeStream(m_Stream);
            m_Stream = NULL;
        }
    }

    STDMETHODIMP RegisterNotificationEvent(PKEVENT NotificationEvent) {
        if (NotificationEvent == NULL) {
            return STATUS_INVALID_PARAMETER;
        }

        BOOLEAN alreadyCached = FALSE;
        for (ULONG i = 0; i < m_CachedEventCount; i++) {
            if (m_CachedEvents[i] == (PVOID)NotificationEvent) {
                alreadyCached = TRUE;
                break;
            }
        }
        if (!alreadyCached) {
            if (m_CachedEventCount < SOF_WAVERT_MAX_EVENTS) {
                m_CachedEvents[m_CachedEventCount++] = (PVOID)NotificationEvent;
            } else {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }

        NTSTATUS status = STATUS_SUCCESS;
        if (m_Stream != NULL) {
            status = SofWaveRtRegisterNotificationEvent(m_Stream, (PVOID)NotificationEvent);
        }

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[MiniportStream] RegisterNotificationEvent: evt=%p, cachedCount=%u, stream=%p, status=0x%08X\n",
            NotificationEvent, m_CachedEventCount, m_Stream, status);
        SofLogBreadcrumb(msg);

        return status;
    }

    STDMETHODIMP UnregisterNotificationEvent(PKEVENT NotificationEvent) {
        if (NotificationEvent == NULL) {
            return STATUS_INVALID_PARAMETER;
        }

        for (ULONG i = 0; i < m_CachedEventCount; i++) {
            if (m_CachedEvents[i] == (PVOID)NotificationEvent) {
                for (ULONG j = i; j + 1 < m_CachedEventCount; j++) {
                    m_CachedEvents[j] = m_CachedEvents[j + 1];
                }
                m_CachedEventCount--;
                break;
            }
        }

        NTSTATUS status = STATUS_SUCCESS;
        if (m_Stream != NULL) {
            status = SofWaveRtUnregisterNotificationEvent(m_Stream, (PVOID)NotificationEvent);
        }

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[MiniportStream] UnregisterNotificationEvent: evt=%p, cachedCount=%u, stream=%p, status=0x%08X\n",
            NotificationEvent, m_CachedEventCount, m_Stream, status);
        SofLogBreadcrumb(msg);

        return status;
    }
};

/* =========================================================================
 * CMiniportWaveRTSof (Miniport Adapter)
 * ========================================================================= */

/**
 * @class CMiniportWaveRTSof
 * @brief Main WaveRT audio miniport driver object implementing IMiniportWaveRT.
 *
 * Exposes audio filter topology, pin factories, audio volume/mute control nodes,
 * and instantiates CMiniportWaveRTStreamSof objects on demand.
 */
class CMiniportWaveRTSof : public IMiniportWaveRT {
private:
    LONG                m_RefCount;
    PSOF_DEVICE_CONTEXT m_DevCtx;
    PPORTWAVERT         m_Port;

    class CPowerNotify : public IPowerNotify {
    private:
        CMiniportWaveRTSof* m_Parent;
    public:
        CPowerNotify(CMiniportWaveRTSof* Parent) : m_Parent(Parent) {}

        /* IUnknown */
        STDMETHODIMP QueryInterface(REFIID riid, PVOID* ppv) {
            return m_Parent->QueryInterface(riid, ppv);
        }
        STDMETHODIMP_(ULONG) AddRef() {
            return m_Parent->AddRef();
        }
        STDMETHODIMP_(ULONG) Release() {
            return m_Parent->Release();
        }

        /* IPowerNotify */
        STDMETHODIMP_(void) PowerChangeNotify(POWER_STATE PowerState) {
            m_Parent->PowerChangeNotify(PowerState);
        }
    } m_PowerNotify;

public:
    PPORTEVENTS         m_PortEvents;

    CMiniportWaveRTSof(PSOF_DEVICE_CONTEXT DevCtx)
        : m_RefCount(1), m_DevCtx(DevCtx), m_Port(NULL), m_PowerNotify(this), m_PortEvents(NULL)
    {}

    ~CMiniportWaveRTSof() {
        if (m_DevCtx != NULL && m_DevCtx->PortEvents == (PVOID)m_PortEvents) {
            m_DevCtx->PortEvents = NULL;
        }
        if (m_PortEvents != NULL) {
            m_PortEvents->Release();
            m_PortEvents = NULL;
        }
        if (m_Port != NULL) {
            m_Port->Release();
            m_Port = NULL;
        }
    }

    PSOF_DEVICE_CONTEXT GetDevCtx() const { return m_DevCtx; }
    PPORTWAVERT GetPort() const { return m_Port; }
    PPORTEVENTS GetPortEvents() const { return m_PortEvents; }

    /* IUnknown */
    STDMETHODIMP QueryInterface(REFIID riid, PVOID* ppv) {
        if (ppv == NULL) return STATUS_INVALID_PARAMETER;

        if (IsEqualGUID(riid, IID_IUnknown) ||
            IsEqualGUID(riid, IID_IMiniport) ||
            IsEqualGUID(riid, IID_IMiniportWaveRT))
        {
            *ppv = static_cast<IMiniportWaveRT*>(this);
            AddRef();
            return STATUS_SUCCESS;
        }

        if (IsEqualGUID(riid, IID_IPowerNotify))
        {
            *ppv = static_cast<IPowerNotify*>(&m_PowerNotify);
            AddRef();
            return STATUS_SUCCESS;
        }
        *ppv = NULL;
        return STATUS_NOINTERFACE;
    }

    /* IPowerNotify Handler */
    void PowerChangeNotify(POWER_STATE PowerState) {
        char pmsg[128];
        RtlStringCbPrintfA(pmsg, sizeof(pmsg),
            "[PortCls] PowerChangeNotify: DeviceState=D%u, SystemState=S%u\n",
            PowerState.DeviceState, PowerState.SystemState);
        SofLogBreadcrumb(pmsg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", pmsg);
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_RefCount);
    }

    STDMETHODIMP_(ULONG) Release() {
        LONG count = InterlockedDecrement(&m_RefCount);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    /* IMiniport */
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR* Description) {
        SofLogBreadcrumb("[Miniport] GetDescription called!\n");
        if (Description == NULL) return STATUS_INVALID_PARAMETER;
        *Description = (PPCFILTER_DESCRIPTOR)&FilterDescriptor;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP DataRangeIntersection(
        ULONG PinId,
        PKSDATARANGE DataRange,
        PKSDATARANGE MatchingDataRange,
        ULONG OutputBufferLength,
        PVOID ResultantFormat,
        PULONG ResultantFormatLength
    ) {
        UNREFERENCED_PARAMETER(PinId);

        if (DataRange == NULL || MatchingDataRange == NULL || ResultantFormatLength == NULL) {
            return STATUS_INVALID_PARAMETER;
        }

        /* Verify MajorFormat is Audio or Wildcard */
        if (!IsEqualGUID(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
            !IsEqualGUID(DataRange->MajorFormat, GUID_NULL)) {
            return STATUS_NO_MATCH;
        }

        /* Verify SubFormat is PCM or Wildcard */
        if (!IsEqualGUID(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
            !IsEqualGUID(DataRange->SubFormat, GUID_NULL)) {
            return STATUS_NO_MATCH;
        }

        /* Verify Specifier is WaveFormatEx, DSOUND, or Wildcard */
        if (!IsEqualGUID(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
            !IsEqualGUID(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_DSOUND) &&
            !IsEqualGUID(DataRange->Specifier, GUID_NULL)) {
            return STATUS_NO_MATCH;
        }

        /* Verify audio range structure size */
        if (DataRange->FormatSize < sizeof(KSDATARANGE) ||
            MatchingDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO)) {
            return STATUS_NO_MATCH;
        }

        PKSDATARANGE_AUDIO pinRange = (PKSDATARANGE_AUDIO)MatchingDataRange;
        ULONG channels = 2;
        ULONG sampleRate = 48000;
        ULONG bitsPerSample = 16;
        BOOLEAN useExtensible = FALSE;
        ULONG channelMask = KSAUDIO_SPEAKER_STEREO;

        if (DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
            PKSDATARANGE_AUDIO clientRange = (PKSDATARANGE_AUDIO)DataRange;

            /* Channel intersection */
            ULONG minChan = (clientRange->MaximumChannels < pinRange->MaximumChannels) ?
                             clientRange->MaximumChannels : pinRange->MaximumChannels;
            if (minChan < 1) {
                return STATUS_NO_MATCH;
            }

            if (clientRange->MaximumChannels >= 8 && pinRange->MaximumChannels >= 8) {
                channels = 8;
                channelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
                useExtensible = TRUE;
            } else if (clientRange->MaximumChannels >= 6 && pinRange->MaximumChannels >= 6) {
                channels = 6;
                channelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
                useExtensible = TRUE;
            } else if (clientRange->MaximumChannels >= 4 && pinRange->MaximumChannels >= 4) {
                channels = 4;
                channelMask = KSAUDIO_SPEAKER_QUAD;
                useExtensible = TRUE;
            } else if (minChan >= 2) {
                channels = 2;
                channelMask = KSAUDIO_SPEAKER_STEREO;
            } else {
                channels = 1;
                channelMask = KSAUDIO_SPEAKER_MONO;
            }

            /* Sample frequency intersection: 32k to 192k */
            ULONG minFreq = (clientRange->MinimumSampleFrequency > pinRange->MinimumSampleFrequency) ?
                             clientRange->MinimumSampleFrequency : pinRange->MinimumSampleFrequency;
            ULONG maxFreq = (clientRange->MaximumSampleFrequency < pinRange->MaximumSampleFrequency) ?
                             clientRange->MaximumSampleFrequency : pinRange->MaximumSampleFrequency;
            if (minFreq > maxFreq) {
                return STATUS_NO_MATCH;
            }

            /* If client requested a single specific sample rate, match it */
            if (clientRange->MinimumSampleFrequency == clientRange->MaximumSampleFrequency) {
                sampleRate = clientRange->MinimumSampleFrequency;
            } else if (48000 >= minFreq && 48000 <= maxFreq) {
                sampleRate = 48000;
            } else if (44100 >= minFreq && 44100 <= maxFreq) {
                sampleRate = 44100;
            } else if (96000 >= minFreq && 96000 <= maxFreq) {
                sampleRate = 96000;
            } else if (192000 >= minFreq && 192000 <= maxFreq) {
                sampleRate = 192000;
            } else if (88200 >= minFreq && 88200 <= maxFreq) {
                sampleRate = 88200;
            } else if (176400 >= minFreq && 176400 <= maxFreq) {
                sampleRate = 176400;
            } else {
                sampleRate = maxFreq;
            }

            /* Bits per sample intersection: 16, 20, 24, 32 */
            ULONG minBits = (clientRange->MinimumBitsPerSample > pinRange->MinimumBitsPerSample) ?
                             clientRange->MinimumBitsPerSample : pinRange->MinimumBitsPerSample;
            ULONG maxBits = (clientRange->MaximumBitsPerSample < pinRange->MaximumBitsPerSample) ?
                             clientRange->MaximumBitsPerSample : pinRange->MaximumBitsPerSample;
            if (minBits > maxBits) {
                return STATUS_NO_MATCH;
            }

            if (clientRange->MinimumBitsPerSample == clientRange->MaximumBitsPerSample) {
                bitsPerSample = clientRange->MinimumBitsPerSample;
            } else if (24 >= minBits && 24 <= maxBits && clientRange->MinimumBitsPerSample >= 24) {
                bitsPerSample = 24;
            } else if (16 >= minBits && 16 <= maxBits) {
                bitsPerSample = 16;
            } else {
                bitsPerSample = minBits;
            }
        }

        if (channels > 2 || bitsPerSample > 16) {
            useExtensible = TRUE;
        }

        ULONG formatSize = useExtensible ?
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) : sizeof(KSDATAFORMAT_WAVEFORMATEX);
        *ResultantFormatLength = formatSize;

        if (OutputBufferLength == 0) {
            return STATUS_BUFFER_OVERFLOW;
        }

        if (OutputBufferLength < formatSize) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (ResultantFormat == NULL) {
            return STATUS_INVALID_PARAMETER;
        }

        RtlZeroMemory(ResultantFormat, formatSize);

        if (useExtensible) {
            PKSDATAFORMAT_WAVEFORMATEXTENSIBLE resFormatExt = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)ResultantFormat;
            ULONG bytesPerSample = (bitsPerSample == 24) ? 4 : ((bitsPerSample + 7) / 8); /* 24-bit in 32-bit container */

            resFormatExt->DataFormat.FormatSize = formatSize;
            resFormatExt->DataFormat.Flags = 0;
            resFormatExt->DataFormat.SampleSize = (WORD)(channels * bytesPerSample);
            resFormatExt->DataFormat.Reserved = 0;
            resFormatExt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
            resFormatExt->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            resFormatExt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

            resFormatExt->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            resFormatExt->WaveFormatExt.Format.nChannels = (WORD)channels;
            resFormatExt->WaveFormatExt.Format.nSamplesPerSec = sampleRate;
            resFormatExt->WaveFormatExt.Format.wBitsPerSample = (WORD)(bytesPerSample * 8);
            resFormatExt->WaveFormatExt.Format.nBlockAlign = (WORD)(channels * bytesPerSample);
            resFormatExt->WaveFormatExt.Format.nAvgBytesPerSec = sampleRate * resFormatExt->WaveFormatExt.Format.nBlockAlign;
            resFormatExt->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            resFormatExt->WaveFormatExt.Samples.wValidBitsPerSample = (WORD)bitsPerSample;
            resFormatExt->WaveFormatExt.dwChannelMask = channelMask;
            resFormatExt->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        } else {
            PKSDATAFORMAT_WAVEFORMATEX resFormat = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
            resFormat->DataFormat.FormatSize = formatSize;
            resFormat->DataFormat.Flags = 0;
            resFormat->DataFormat.SampleSize = (WORD)((channels * bitsPerSample) / 8);
            resFormat->DataFormat.Reserved = 0;
            resFormat->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
            resFormat->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            resFormat->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

            resFormat->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
            resFormat->WaveFormatEx.nChannels = (WORD)channels;
            resFormat->WaveFormatEx.nSamplesPerSec = sampleRate;
            resFormat->WaveFormatEx.wBitsPerSample = (WORD)bitsPerSample;
            resFormat->WaveFormatEx.nBlockAlign = (WORD)((channels * bitsPerSample) / 8);
            resFormat->WaveFormatEx.nAvgBytesPerSec = sampleRate * resFormat->WaveFormatEx.nBlockAlign;
            resFormat->WaveFormatEx.cbSize = 0;
        }

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[Miniport] DataRangeIntersection: Pin=%u, %uHz, %uch, %ubit (ext=%u)\n",
            PinId, sampleRate, channels, bitsPerSample, useExtensible ? 1 : 0);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    /* IMiniportWaveRT */
    STDMETHODIMP Init(
        PUNKNOWN UnknownAdapter,
        PRESOURCELIST ResourceList,
        PPORTWAVERT Port
    ) {
        SofLogBreadcrumb("[Miniport] Init called!\n");
        UNREFERENCED_PARAMETER(UnknownAdapter);
        UNREFERENCED_PARAMETER(ResourceList);

        if (Port == NULL) return STATUS_INVALID_PARAMETER;
        m_Port = Port;
        m_Port->AddRef();

        /* Query IPortEvents for dynamic jack presence notifications */
        m_PortEvents = NULL;
        Port->QueryInterface(IID_IPortEvents, (PVOID*)&m_PortEvents);
        if (m_PortEvents != NULL) {
            SofLogBreadcrumb("[Miniport] IPortEvents acquired successfully!\n");
            if (m_DevCtx != NULL) {
                m_DevCtx->PortEvents = (PVOID)m_PortEvents;
            }
        } else {
            SofLogBreadcrumb("[Miniport] IPortEvents NOT supported by Port object\n");
        }

        return STATUS_SUCCESS;
    }

    STDMETHODIMP NewStream(
        PMINIPORTWAVERTSTREAM* Stream,
        PPORTWAVERTSTREAM PortStream,
        ULONG Pin,
        BOOLEAN Capture,
        PKSDATAFORMAT DataFormat
    ) {
        SofLogBreadcrumb("[Miniport] NewStream called!\n");
        UNREFERENCED_PARAMETER(PortStream);

        if (Stream == NULL) return STATUS_INVALID_PARAMETER;

        CMiniportWaveRTStreamSof* streamObj = new (POOL_FLAG_NON_PAGED, SOF_DEVICE_TAG)
            CMiniportWaveRTStreamSof(m_DevCtx, Pin, Capture);

        if (streamObj == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (DataFormat != NULL) {
            streamObj->SetFormat(DataFormat);
        }

        *Stream = streamObj;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP GetDeviceDescription(PDEVICE_DESCRIPTION DeviceDescription) {
        SofLogBreadcrumb("[Miniport] GetDeviceDescription called!\n");
        if (DeviceDescription == NULL) return STATUS_INVALID_PARAMETER;
        RtlZeroMemory(DeviceDescription, sizeof(DEVICE_DESCRIPTION));
        DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
        DeviceDescription->Master = TRUE;
        DeviceDescription->ScatterGather = TRUE;
        DeviceDescription->DemandMode = FALSE;
        DeviceDescription->AutoInitialize = FALSE;
        DeviceDescription->Dma32BitAddresses = TRUE;
        DeviceDescription->InterfaceType = PCIBus;
        DeviceDescription->MaximumLength = 0xFFFFFFFF;
        return STATUS_SUCCESS;
    }
};

/* =========================================================================
 * KS Property Handlers (Volume, Mute, PeakMeter)
 * ========================================================================= */

static NTSTATUS PropertyHandler_VolumeLevel(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    if (PropertyRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    CMiniportWaveRTSof* miniport = (CMiniportWaveRTSof*)PropertyRequest->MajorTarget;
    PSOF_DEVICE_CONTEXT devCtx = (miniport != NULL) ? miniport->GetDevCtx() : NULL;
    PSOF_WAVERT_STREAM stream = NULL;

    if (PropertyRequest->MinorTarget != NULL) {
        CMiniportWaveRTStreamSof* streamObj = (CMiniportWaveRTStreamSof*)PropertyRequest->MinorTarget;
        stream = streamObj->GetStream();
    } else if (devCtx != NULL) {
        stream = (PSOF_WAVERT_STREAM)devCtx->PlaybackStream;
    }

    LONG channel = -1;
    if (PropertyRequest->Instance != NULL && PropertyRequest->InstanceSize >= sizeof(KSNODEPROPERTY_AUDIO_CHANNEL)) {
        PKSNODEPROPERTY_AUDIO_CHANNEL chanProp = (PKSNODEPROPERTY_AUDIO_CHANNEL)PropertyRequest->Instance;
        channel = chanProp->Channel;
    }

    /* Basic Support Query */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_BASICSUPPORT) {
        if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
            *(PULONG)PropertyRequest->Value =
                PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;

            if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
                PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;
                desc->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
                desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);
                desc->PropTypeSet.Set = KSPROPTYPESETID_General;
                desc->PropTypeSet.Id = 3; /* VT_I4 */
                desc->PropTypeSet.Flags = 0;
                desc->MembersListCount = 1;
                desc->Reserved = 0;

                if (PropertyRequest->ValueSize >= desc->DescriptionSize) {
                    PKSPROPERTY_MEMBERSHEADER members = (PKSPROPERTY_MEMBERSHEADER)(desc + 1);
                    members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
                    members->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
                    members->MembersCount = 1;
                    members->Flags = 0;

                    PKSPROPERTY_STEPPING_LONG range = (PKSPROPERTY_STEPPING_LONG)(members + 1);
                    range->Bounds.SignedMinimum = SOF_VOLUME_MIN_DB_UNITS; /* -96 dB */
                    range->Bounds.SignedMaximum = SOF_VOLUME_MAX_DB_UNITS; /* 0 dB */
                    range->SteppingDelta = SOF_VOLUME_STEP_DB_UNITS;        /* 0.5 dB */
                    range->Reserved = 0;
                    PropertyRequest->ValueSize = desc->DescriptionSize;
                } else {
                    PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
                }
            } else {
                PropertyRequest->ValueSize = sizeof(ULONG);
            }
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Get Volume */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_GET) {
        if (PropertyRequest->ValueSize < sizeof(LONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        LONG volDb = 0;
        if (stream != NULL) {
            SofWaveRtGetVolume(stream, channel, &volDb);
        } else if (devCtx != NULL) {
            ULONG ep = devCtx->HdaCodecPresent ? SOF_ENDPOINT_HDA_ANALOG : SOF_ENDPOINT_HDMI_1;
            ULONG chIdx = (channel == 1) ? 1 : 0;
            volDb = devCtx->DefaultVolumeLevelDb[ep][chIdx];
        }

        *(PLONG)PropertyRequest->Value = volDb;
        PropertyRequest->ValueSize = sizeof(LONG);

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[PropertyHandler] Volume GET: Chan=%ld, Val=%ld dB, stream=%p\n",
            channel, volDb, stream);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    /* Set Volume */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_SET) {
        if (PropertyRequest->ValueSize < sizeof(LONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        LONG volDb = *(PLONG)PropertyRequest->Value;
        if (stream != NULL) {
            SofWaveRtSetVolume(stream, channel, volDb);
        } else if (devCtx != NULL) {
            ULONG ep = devCtx->HdaCodecPresent ? SOF_ENDPOINT_HDA_ANALOG : SOF_ENDPOINT_HDMI_1;
            if (channel == 0 || channel == -1) {
                devCtx->DefaultVolumeLevelDb[ep][0] = volDb;
            }
            if (channel == 1 || channel == -1) {
                devCtx->DefaultVolumeLevelDb[ep][1] = volDb;
            }
        }

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[PropertyHandler] Volume SET: Chan=%ld, Val=%ld dB, stream=%p\n",
            channel, volDb, stream);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS PropertyHandler_Mute(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    if (PropertyRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    CMiniportWaveRTSof* miniport = (CMiniportWaveRTSof*)PropertyRequest->MajorTarget;
    PSOF_DEVICE_CONTEXT devCtx = (miniport != NULL) ? miniport->GetDevCtx() : NULL;
    PSOF_WAVERT_STREAM stream = NULL;

    if (PropertyRequest->MinorTarget != NULL) {
        CMiniportWaveRTStreamSof* streamObj = (CMiniportWaveRTStreamSof*)PropertyRequest->MinorTarget;
        stream = streamObj->GetStream();
    } else if (devCtx != NULL) {
        stream = (PSOF_WAVERT_STREAM)devCtx->PlaybackStream;
    }

    /* Basic Support Query */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_BASICSUPPORT) {
        if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
            *(PULONG)PropertyRequest->Value =
                PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;

            if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
                PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;
                desc->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
                desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
                desc->PropTypeSet.Set = KSPROPTYPESETID_General;
                desc->PropTypeSet.Id = 11; /* VT_BOOL */
                desc->PropTypeSet.Flags = 0;
                desc->MembersListCount = 0;
                desc->Reserved = 0;
                PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            } else {
                PropertyRequest->ValueSize = sizeof(ULONG);
            }
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Get Mute */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_GET) {
        if (PropertyRequest->ValueSize < sizeof(BOOL)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        BOOLEAN isMuted = FALSE;
        if (stream != NULL) {
            SofWaveRtGetMute(stream, &isMuted);
        } else if (devCtx != NULL) {
            ULONG ep = devCtx->HdaCodecPresent ? SOF_ENDPOINT_HDA_ANALOG : SOF_ENDPOINT_HDMI_1;
            isMuted = devCtx->DefaultMute[ep];
        }

        *(PBOOL)PropertyRequest->Value = isMuted ? TRUE : FALSE;
        PropertyRequest->ValueSize = sizeof(BOOL);

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[PropertyHandler] Mute GET: Mute=%d, stream=%p\n",
            (int)isMuted, stream);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    /* Set Mute */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_SET) {
        if (PropertyRequest->ValueSize < sizeof(BOOL)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        BOOL muteVal = *(PBOOL)PropertyRequest->Value;
        BOOLEAN isMuted = (muteVal != FALSE);

        if (stream != NULL) {
            SofWaveRtSetMute(stream, isMuted);
        } else if (devCtx != NULL) {
            ULONG ep = devCtx->HdaCodecPresent ? SOF_ENDPOINT_HDA_ANALOG : SOF_ENDPOINT_HDMI_1;
            devCtx->DefaultMute[ep] = isMuted;
        }

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[PropertyHandler] Mute SET: Mute=%d, stream=%p\n",
            (int)isMuted, stream);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS PropertyHandler_PeakMeter(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    if (PropertyRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    CMiniportWaveRTSof* miniport = (CMiniportWaveRTSof*)PropertyRequest->MajorTarget;
    PSOF_DEVICE_CONTEXT devCtx = (miniport != NULL) ? miniport->GetDevCtx() : NULL;
    PSOF_WAVERT_STREAM stream = NULL;

    if (PropertyRequest->MinorTarget != NULL) {
        CMiniportWaveRTStreamSof* streamObj = (CMiniportWaveRTStreamSof*)PropertyRequest->MinorTarget;
        stream = streamObj->GetStream();
    } else if (devCtx != NULL) {
        stream = (PSOF_WAVERT_STREAM)devCtx->PlaybackStream;
    }

    LONG channel = 0;
    if (PropertyRequest->InstanceSize >= sizeof(KSNODEPROPERTY_AUDIO_CHANNEL)) {
        PKSNODEPROPERTY_AUDIO_CHANNEL chanProp = (PKSNODEPROPERTY_AUDIO_CHANNEL)PropertyRequest->Instance;
        channel = chanProp->Channel;
    }

    /* Basic Support Query */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_BASICSUPPORT) {
        if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
            *(PULONG)PropertyRequest->Value = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;
            PropertyRequest->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Get Peak Meter */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_GET) {
        if (PropertyRequest->ValueSize < sizeof(LONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        LONG peakDb = SOF_VOLUME_MIN_DB_UNITS;
        if (stream != NULL) {
            SofWaveRtGetPeakMeter(stream, channel, &peakDb);
        }

        *(PLONG)PropertyRequest->Value = peakDb;
        PropertyRequest->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

/* =========================================================================
 * Automation & Filter Topology Descriptors
 * ========================================================================= */

/* Property table and automation table for Volume node */
static const PCPROPERTY_ITEM VolumeProperties[] = {
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        PropertyHandler_VolumeLevel
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(VolumeAutomationTable, VolumeProperties);

/* Property table and automation table for Mute node */
static const PCPROPERTY_ITEM MuteProperties[] = {
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        PropertyHandler_Mute
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(MuteAutomationTable, MuteProperties);

/* Property table and automation table for PeakMeter node */
static const PCPROPERTY_ITEM PeakMeterProperties[] = {
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_PEAKMETER,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        PropertyHandler_PeakMeter
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_PEAKMETER2,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        PropertyHandler_PeakMeter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(PeakMeterAutomationTable, PeakMeterProperties);

/* =========================================================================
 * Filter-Level KS Property Handlers (KSPROPSETID_Jack Dispatch)
 * ========================================================================= */

#ifndef JACKDESC2_PRESENCE_DETECT_CAPABILITY
#define JACKDESC2_PRESENCE_DETECT_CAPABILITY 0x00000001
#endif
#ifndef JACKDESC2_DYNAMIC_FORMAT_CHANGE_CAPABILITY
#define JACKDESC2_DYNAMIC_FORMAT_CHANGE_CAPABILITY 0x00000002
#endif

static ULONG GetPinIdFromPropertyRequest(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    if (PropertyRequest == NULL) {
        return (ULONG)-1;
    }

    /* 1. Check PropertyRequest->Instance (PortCls-allocated safe kernel buffer) */
    if (PropertyRequest->Instance != NULL && PropertyRequest->InstanceSize >= sizeof(ULONG)) {
        PULONG pUlong = (PULONG)PropertyRequest->Instance;
        ULONG count = PropertyRequest->InstanceSize / sizeof(ULONG);

        /* Case 1a: PortCls passed full KSP_PIN (32 bytes = 8 ULONGs), PinId is at index 6 (byte offset 24) */
        if (count >= 7 && pUlong[6] <= 16) {
            return pUlong[6];
        }

        /* Case 1b: PortCls stripped KSPROPERTY (24 bytes), PinId is at offset 0 */
        if (count >= 1 && pUlong[0] <= 16) {
            return pUlong[0];
        }
    }

    /* 2. Check from Irp input buffer if available */
    if (PropertyRequest->Irp != NULL) {
        __try {
            PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(PropertyRequest->Irp);
            if (irpSp != NULL) {
                PVOID inBuf = irpSp->Parameters.DeviceIoControl.Type3InputBuffer;
                if (inBuf == NULL) {
                    inBuf = PropertyRequest->Irp->AssociatedIrp.SystemBuffer;
                }
                ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
                if (inBuf != NULL && inLen >= sizeof(KSPROPERTY) + sizeof(ULONG)) {
                    PKSP_PIN pPin = (PKSP_PIN)inBuf;
                    if (pPin->PinId <= 16) {
                        return pPin->PinId;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* Ignore probe failure */
        }
    }

    /* 3. Check Node if populated by PortCls */
    if (PropertyRequest->Node != (ULONG)-1 && PropertyRequest->Node <= 16) {
        return PropertyRequest->Node;
    }

    return (ULONG)-1;
}

static NTSTATUS FilterPropertyHandler_JackDescription(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    if (PropertyRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    CMiniportWaveRTSof* miniport = (CMiniportWaveRTSof*)PropertyRequest->MajorTarget;
    PSOF_DEVICE_CONTEXT devCtx = (miniport != NULL) ? miniport->GetDevCtx() : NULL;
    ULONG pinId = GetPinIdFromPropertyRequest(PropertyRequest);
    ULONG hdmiIdx = (ULONG)-1;

    if (pinId == 5) {
        hdmiIdx = 0;
    } else if (pinId == 7) {
        hdmiIdx = 1;
    } else if (pinId == 9) {
        hdmiIdx = 2;
    }

    ULONG d0 = 0, d6 = 0;
    if (PropertyRequest->Instance != NULL) {
        PULONG pU = (PULONG)PropertyRequest->Instance;
        ULONG cnt = PropertyRequest->InstanceSize / 4;
        if (cnt > 0) d0 = pU[0];
        if (cnt > 6) d6 = pU[6];
    }

    char msg[128];
    RtlStringCbPrintfA(msg, sizeof(msg),
        "[FilterJack] Desc: Verb=0x%08X, Pin=%u, hdmiIdx=%u, Sz=%u, [0]=%u,[6]=%u\n",
        PropertyRequest->Verb, pinId, hdmiIdx, PropertyRequest->InstanceSize, d0, d6);
    SofLogBreadcrumb(msg);

    if (hdmiIdx >= 3) {
        /* Non-HDMI pins (e.g. Speakers, Mic) do not support jack presence detect */
        return STATUS_NOT_FOUND;
    }

    if (devCtx != NULL && !devCtx->IDispCodecPresent && devCtx->Bar0VirtualAddress) {
        USHORT wakests = readw((PUCHAR)devCtx->Bar0VirtualAddress + SOF_HDA_WAKESTS);
        if (wakests & SOF_HDA_WAKESTS_SDIN2) {
            devCtx->IDispCodecPresent = TRUE;
            if (NT_SUCCESS(SofHdaProbeDisplayCodec(devCtx, 2, &devCtx->GfxCodecVendorId, &devCtx->GfxCodecRevisionId))) {
                SofHdaInitDisplayCodec(devCtx, 2);
            }
        }
    }

    BOOLEAN connected = (devCtx != NULL) ? devCtx->DisplayConnected[hdmiIdx] : FALSE;

    /* Basic Support Query */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_BASICSUPPORT) {
        if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
            *(PULONG)PropertyRequest->Value = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;

            if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
                PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;
                desc->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
                desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
                desc->PropTypeSet.Set = KSPROPTYPESETID_General;
                desc->PropTypeSet.Id = 0;
                desc->PropTypeSet.Flags = 0;
                desc->MembersListCount = 0;
                desc->Reserved = 0;
                PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            } else {
                PropertyRequest->ValueSize = sizeof(ULONG);
            }
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Get Jack Description */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_GET) {
        ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);

        if (PropertyRequest->ValueSize == 0) {
            PropertyRequest->ValueSize = cbNeeded;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (PropertyRequest->ValueSize < cbNeeded) {
            PropertyRequest->ValueSize = cbNeeded;
            return STATUS_BUFFER_TOO_SMALL;
        }

        PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
        PKSJACK_DESCRIPTION jackDesc = (PKSJACK_DESCRIPTION)(pMI + 1);

        pMI->Size = cbNeeded;
        pMI->Count = 1;

        RtlZeroMemory(jackDesc, sizeof(KSJACK_DESCRIPTION));

        jackDesc->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
        jackDesc->Color = 0;
        jackDesc->ConnectionType = eConnTypeOtherDigital;
        jackDesc->GeoLocation = eGeoLocHDMI;
        jackDesc->PortConnection = ePortConnJack;
        jackDesc->IsConnected = (devCtx != NULL && devCtx->DisplayConnected[hdmiIdx]) ? TRUE : FALSE;

        PropertyRequest->ValueSize = cbNeeded;

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[FilterJack] Description GET result: Pin=%u, hdmiIdx=%u, PortConn=%u, Connected=%d\n",
            pinId, hdmiIdx, (unsigned int)jackDesc->PortConnection, (int)jackDesc->IsConnected);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS FilterPropertyHandler_JackDescription2(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    if (PropertyRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG pinId = GetPinIdFromPropertyRequest(PropertyRequest);
    ULONG hdmiIdx = (ULONG)-1;
    if (pinId == 5) hdmiIdx = 0;
    else if (pinId == 7) hdmiIdx = 1;
    else if (pinId == 9) hdmiIdx = 2;

    if (hdmiIdx >= 3) {
        return STATUS_NOT_FOUND;
    }

    /* Basic Support Query */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_BASICSUPPORT) {
        if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
            *(PULONG)PropertyRequest->Value = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;

            if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
                PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;
                desc->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
                desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
                desc->PropTypeSet.Set = KSPROPTYPESETID_General;
                desc->PropTypeSet.Id = 0;
                desc->PropTypeSet.Flags = 0;
                desc->MembersListCount = 0;
                desc->Reserved = 0;
                PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            } else {
                PropertyRequest->ValueSize = sizeof(ULONG);
            }
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Get Jack Description 2 */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_GET) {
        ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION2);

        if (PropertyRequest->ValueSize == 0) {
            PropertyRequest->ValueSize = cbNeeded;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (PropertyRequest->ValueSize < cbNeeded) {
            PropertyRequest->ValueSize = cbNeeded;
            return STATUS_BUFFER_TOO_SMALL;
        }

        PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
        PKSJACK_DESCRIPTION2 jackDesc2 = (PKSJACK_DESCRIPTION2)(pMI + 1);

        pMI->Size = cbNeeded;
        pMI->Count = 1;

        CMiniportWaveRTSof* miniport = (CMiniportWaveRTSof*)PropertyRequest->MajorTarget;
        PSOF_DEVICE_CONTEXT devCtx = (miniport != NULL) ? miniport->GetDevCtx() : NULL;
        BOOLEAN connected = (devCtx != NULL) ? devCtx->DisplayConnected[hdmiIdx] : FALSE;

        RtlZeroMemory(jackDesc2, sizeof(KSJACK_DESCRIPTION2));

        jackDesc2->DeviceStateInfo = 0;
        jackDesc2->JackCapabilities = JACKDESC2_PRESENCE_DETECT_CAPABILITY | JACKDESC2_DYNAMIC_FORMAT_CHANGE_CAPABILITY;

        PropertyRequest->ValueSize = cbNeeded;

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[FilterJack] Desc2 GET: Pin=%u, hdmiIdx=%u, Connected=%d, Caps=0x%08X\n",
            pinId, hdmiIdx, (int)connected, jackDesc2->JackCapabilities);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS FilterPropertyHandler_JackSinkInfo(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    if (PropertyRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    CMiniportWaveRTSof* miniport = (CMiniportWaveRTSof*)PropertyRequest->MajorTarget;
    PSOF_DEVICE_CONTEXT devCtx = (miniport != NULL) ? miniport->GetDevCtx() : NULL;
    ULONG pinId = GetPinIdFromPropertyRequest(PropertyRequest);

    ULONG hdmiIdx = (ULONG)-1;
    if (pinId == 5) hdmiIdx = 0;
    else if (pinId == 7) hdmiIdx = 1;
    else if (pinId == 9) hdmiIdx = 2;

    if (hdmiIdx >= 3) {
        return STATUS_NOT_FOUND;
    }

    if (devCtx != NULL && !devCtx->IDispCodecPresent && devCtx->Bar0VirtualAddress) {
        USHORT wakests = readw((PUCHAR)devCtx->Bar0VirtualAddress + SOF_HDA_WAKESTS);
        if (wakests & SOF_HDA_WAKESTS_SDIN2) {
            devCtx->IDispCodecPresent = TRUE;
            if (NT_SUCCESS(SofHdaProbeDisplayCodec(devCtx, 2, &devCtx->GfxCodecVendorId, &devCtx->GfxCodecRevisionId))) {
                SofHdaInitDisplayCodec(devCtx, 2);
            }
        }
    }

    /* Basic Support Query */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_BASICSUPPORT) {
        if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
            *(PULONG)PropertyRequest->Value = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;

            if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
                PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;
                desc->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
                desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
                desc->PropTypeSet.Set = KSPROPTYPESETID_General;
                desc->PropTypeSet.Id = 0;
                desc->PropTypeSet.Flags = 0;
                desc->MembersListCount = 0;
                desc->Reserved = 0;
                PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            } else {
                PropertyRequest->ValueSize = sizeof(ULONG);
            }
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Get Jack Sink Info */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_GET) {
        if (PropertyRequest->ValueSize == 0) {
            PropertyRequest->ValueSize = sizeof(KSJACK_SINK_INFORMATION);
            return STATUS_BUFFER_OVERFLOW;
        }
        if (PropertyRequest->ValueSize < sizeof(KSJACK_SINK_INFORMATION)) {
            PropertyRequest->ValueSize = sizeof(KSJACK_SINK_INFORMATION);
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (devCtx == NULL || !devCtx->DisplayConnected[hdmiIdx]) {
            return STATUS_NOT_FOUND;
        }

        PKSJACK_SINK_INFORMATION sinkInfo = (PKSJACK_SINK_INFORMATION)PropertyRequest->Value;
        RtlZeroMemory(sinkInfo, sizeof(KSJACK_SINK_INFORMATION));

        sinkInfo->ConnType = KSJACK_SINK_CONNECTIONTYPE_HDMI;
        sinkInfo->ManufacturerId = 0;
        sinkInfo->ProductId = 0;
        sinkInfo->AudioLatency = 0;
        sinkInfo->HDCPCapable = FALSE;
        sinkInfo->AICapable = FALSE;

        size_t nameLen = 0;
        RtlStringCchLengthW(devCtx->DisplayMonitorName[hdmiIdx], 32, &nameLen);
        if (nameLen > 31) nameLen = 31;
        sinkInfo->SinkDescriptionLength = (UCHAR)nameLen;
        RtlStringCchCopyW(sinkInfo->SinkDescription, MAX_SINK_DESCRIPTION_NAME_LENGTH, devCtx->DisplayMonitorName[hdmiIdx]);

        PropertyRequest->ValueSize = sizeof(KSJACK_SINK_INFORMATION);

        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[FilterJack] SinkInfo GET: Pin=%u, hdmiIdx=%u, len=%u, name='%ws'\n",
            pinId, hdmiIdx, (unsigned int)nameLen, devCtx->DisplayMonitorName[hdmiIdx]);
        SofLogBreadcrumb(msg);

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS FilterPropertyHandler_JackContainerId(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    UNREFERENCED_PARAMETER(PropertyRequest);
    /* Returning STATUS_NOT_FOUND allows Windows to inherit the device's PnP Container ID */
    return STATUS_NOT_FOUND;
}

static NTSTATUS FilterPropertyHandler_JackDescription3(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    if (PropertyRequest == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG pinId = GetPinIdFromPropertyRequest(PropertyRequest);
    ULONG hdmiIdx = (ULONG)-1;
    if (pinId == 5) hdmiIdx = 0;
    else if (pinId == 7) hdmiIdx = 1;
    else if (pinId == 9) hdmiIdx = 2;

    if (hdmiIdx >= 3) {
        return STATUS_NOT_FOUND;
    }

    /* Basic Support Query */
    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_BASICSUPPORT) {
        if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
            *(PULONG)PropertyRequest->Value = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;
            PropertyRequest->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (PropertyRequest->Verb & PCPROPERTY_ITEM_FLAG_GET) {
        if (PropertyRequest->ValueSize == 0) {
            PropertyRequest->ValueSize = sizeof(ULONG);
            return STATUS_BUFFER_OVERFLOW;
        }
        if (PropertyRequest->ValueSize < sizeof(ULONG)) {
            PropertyRequest->ValueSize = sizeof(ULONG);
            return STATUS_BUFFER_TOO_SMALL;
        }

        *(PULONG)PropertyRequest->Value = 1; /* ConfigId */
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

/* Filter-Level Automation Table */
static const PCPROPERTY_ITEM FilterProperties[] = {
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        FilterPropertyHandler_JackDescription
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        FilterPropertyHandler_JackDescription2
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_SINK_INFO,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        FilterPropertyHandler_JackSinkInfo
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_CONTAINERID,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        FilterPropertyHandler_JackContainerId
    },
    {
        &KSPROPSETID_Jack,
        5, /* KSPROPERTY_JACK_DESCRIPTION3 */
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        FilterPropertyHandler_JackDescription3
    }
};

static NTSTATUS NTAPI FilterEventHandler_PinCapsChange(
    _In_ PPCEVENT_REQUEST EventRequest
)
{
    if (!EventRequest) return STATUS_INVALID_PARAMETER;

    if (EventRequest->Verb & PCEVENT_VERB_SUPPORT) {
        return STATUS_SUCCESS;
    }

    if (EventRequest->Verb & PCEVENT_VERB_ADD) {
        if (!EventRequest->MajorTarget || !EventRequest->EventEntry) {
            return STATUS_INVALID_PARAMETER;
        }

        CMiniportWaveRTSof* miniport = (CMiniportWaveRTSof*)EventRequest->MajorTarget;
        if (miniport && miniport->m_PortEvents) {
            miniport->m_PortEvents->AddEventToEventList(EventRequest->EventEntry);
            char msg[128];
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[PortCls] Added KSEVENT_PINCAPS to EventList on Node=%u\n",
                EventRequest->Node);
            SofLogBreadcrumb(msg);
            return STATUS_SUCCESS;
        }
        return STATUS_UNSUCCESSFUL;
    }

    if (EventRequest->Verb & PCEVENT_VERB_REMOVE) {
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

static const PCEVENT_ITEM BridgePinEvents[] = {
    {
        &KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        PCEVENT_ITEM_FLAG_ENABLE | PCEVENT_ITEM_FLAG_BASICSUPPORT,
        FilterEventHandler_PinCapsChange
    },
    {
        &KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_FORMATCHANGE,
        PCEVENT_ITEM_FLAG_ENABLE | PCEVENT_ITEM_FLAG_BASICSUPPORT,
        FilterEventHandler_PinCapsChange
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(FilterAutomationTable, FilterProperties, BridgePinEvents);
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(BridgePinAutomationTable, FilterProperties, BridgePinEvents);

enum {
    KSNODE_TOPO_VOLUME = 0,
    KSNODE_TOPO_MUTE,
    KSNODE_TOPO_PEAKMETER,
    KSNODE_TOPO_COUNT
};

static const PCNODE_DESCRIPTOR FilterNodes[] = {
    /* Node 0: Volume Control */
    {
        0,                              /* Flags */
        &VolumeAutomationTable,         /* AutomationTable */
        &KSNODETYPE_VOLUME,             /* Type */
        NULL                            /* Name */
    },
    /* Node 1: Mute Control */
    {
        0,                              /* Flags */
        &MuteAutomationTable,           /* AutomationTable */
        &KSNODETYPE_MUTE,               /* Type */
        NULL                            /* Name */
    },
    /* Node 2: Peak Meter */
    {
        0,                              /* Flags */
        &PeakMeterAutomationTable,      /* AutomationTable */
        &KSNODETYPE_PEAKMETER,          /* Type */
        NULL                            /* Name */
    }
};

static const KSDATARANGE_AUDIO PinDataRangesAudio = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    8,      /* MaximumChannels (up to 7.1 surround) */
    16,     /* MinimumBitsPerSample */
    32,     /* MaximumBitsPerSample */
    32000,  /* MinimumSampleFrequency */
    192000  /* MaximumSampleFrequency */
};

static const PKSDATARANGE PinDataRangePointers[] = {
    (PKSDATARANGE)&PinDataRangesAudio
};

static const KSDATARANGE PinDataRangesBridge = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};

static const PKSDATARANGE PinDataRangePointersBridge[] = {
    (PKSDATARANGE)&PinDataRangesBridge
};

static const PCPIN_DESCRIPTOR FilterPins[] = {
    /* Pin 0: Playback Stream (Render / Sink) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    /* Pin 1: Capture Stream (Recording / Source) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    },
    /* Pin 2: Playback Bridge Pin (Bridge to Speaker / Headphone) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            NULL,
            0
        }
    },
    /* Pin 3: Capture Bridge Pin (Bridge from Microphone / LineIn) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE,
            NULL,
            0
        }
    },
    /* Pin 4: HDMI 1 Playback Stream (Render / Sink) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    /* Pin 5: HDMI 1 Playback Bridge Pin (Bridge to HDMI Interface) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HDMI_INTERFACE,
            NULL,
            0
        }
    },
    /* Pin 6: HDMI 2 Playback Stream (Render / Sink) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    /* Pin 7: HDMI 2 Playback Bridge Pin (Bridge to HDMI Interface) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HDMI_INTERFACE,
            NULL,
            0
        }
    },
    /* Pin 8: HDMI 3 Playback Stream (Render / Sink) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    /* Pin 9: HDMI 3 Playback Bridge Pin (Bridge to HDMI Interface) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HDMI_INTERFACE,
            NULL,
            0
        }
    },
    /* Pin 10: PDM / DMIC Capture Stream (Recording / Source) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    },
    /* Pin 11: PDM / DMIC Capture Bridge Pin (Digital Microphone Array) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE_ARRAY,
            NULL,
            0
        }
    },
    /* Pin 12: SSP2 I2S Playback Stream (Render / Sink) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    /* Pin 13: SSP2 I2S Playback Bridge Pin (Bridge to Headphones / Speaker) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HEADPHONES,
            NULL,
            0
        }
    },
    /* Pin 14: SSP2 I2S Capture Stream (Recording / Source) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    },
    /* Pin 15: SSP2 I2S Capture Bridge Pin (Bridge from Headset Microphone) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HEADSET_MICROPHONE,
            NULL,
            0
        }
    },
    /* Pin 16: USB Audio Offload Playback Stream (Render / Sink) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    /* Pin 17: USB Audio Offload Playback Bridge Pin (Bridge to USB DAC / Headset) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HEADSET,
            NULL,
            0
        }
    },
    /* Pin 18: USB Audio Offload Capture Stream (Recording / Source) */
    {
        (ULONG)-1, (ULONG)-1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointers),
            PinDataRangePointers,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    },
    /* Pin 19: USB Audio Offload Capture Bridge Pin (Bridge from USB Microphone) */
    {
        0, 0, 0,
        &BridgePinAutomationTable,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_HEADSET_MICROPHONE,
            NULL,
            0
        }
    }
};

static const PCCONNECTION_DESCRIPTOR FilterConnections[] = {
    /* Playback Path: Pin 0 (Render In) -> Volume Node -> Mute Node -> PeakMeter Node -> Pin 2 (Speaker Bridge) */
    { PCFILTER_NODE, 0, KSNODE_TOPO_VOLUME, KSNODEPIN_STANDARD_IN },
    { KSNODE_TOPO_VOLUME, KSNODEPIN_STANDARD_OUT, KSNODE_TOPO_MUTE, KSNODEPIN_STANDARD_IN },
    { KSNODE_TOPO_MUTE, KSNODEPIN_STANDARD_OUT, KSNODE_TOPO_PEAKMETER, KSNODEPIN_STANDARD_IN },
    { KSNODE_TOPO_PEAKMETER, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, 2 },

    /* Capture Path: Pin 3 (Mic Bridge) -> Pin 1 (Capture Out) */
    { PCFILTER_NODE, 3, PCFILTER_NODE, 1 },

    /* HDMI 1 Playback Path: Pin 4 (HDMI 1 Render In) -> Pin 5 (HDMI 1 Bridge) */
    { PCFILTER_NODE, 4, PCFILTER_NODE, 5 },

    /* HDMI 2 Playback Path: Pin 6 (HDMI 2 Render In) -> Pin 7 (HDMI 2 Bridge) */
    { PCFILTER_NODE, 6, PCFILTER_NODE, 7 },

    /* HDMI 3 Playback Path: Pin 8 (HDMI 3 Render In) -> Pin 9 (HDMI 3 Bridge) */
    { PCFILTER_NODE, 8, PCFILTER_NODE, 9 },

    /* PDM / DMIC Capture Path: Pin 11 (Mic Array Bridge) -> Pin 10 (PDM Capture Out) */
    { PCFILTER_NODE, 11, PCFILTER_NODE, 10 },

    /* SSP2 Playback Path: Pin 12 (SSP Render In) -> Pin 13 (SSP Headphone Bridge) */
    { PCFILTER_NODE, 12, PCFILTER_NODE, 13 },

    /* SSP2 Capture Path: Pin 15 (SSP Headset Mic Bridge) -> Pin 14 (SSP Capture Out) */
    { PCFILTER_NODE, 15, PCFILTER_NODE, 14 },

    /* USB Offload Playback Path: Pin 16 (USB Render In) -> Pin 17 (USB Headset Bridge) */
    { PCFILTER_NODE, 16, PCFILTER_NODE, 17 },

    /* USB Offload Capture Path: Pin 19 (USB Headset Mic Bridge) -> Pin 18 (USB Capture Out) */
    { PCFILTER_NODE, 19, PCFILTER_NODE, 18 }
};

static const GUID FilterCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_RENDER),
    STATICGUIDOF(KSCATEGORY_CAPTURE),
    STATICGUIDOF(KSCATEGORY_REALTIME)
};

const PCFILTER_DESCRIPTOR FilterDescriptor = {
    0,                                      /* Version */
    &FilterAutomationTable,                 /* AutomationTable */
    sizeof(PCPIN_DESCRIPTOR),               /* PinSize */
    SIZEOF_ARRAY(FilterPins),               /* PinCount */
    FilterPins,                             /* Pins */
    sizeof(PCNODE_DESCRIPTOR),              /* NodeSize */
    SIZEOF_ARRAY(FilterNodes),              /* NodeCount */
    FilterNodes,                            /* Nodes */
    SIZEOF_ARRAY(FilterConnections),        /* ConnectionCount */
    FilterConnections,                      /* Connections */
    SIZEOF_ARRAY(FilterCategories),         /* CategoryCount */
    FilterCategories                        /* Categories */
};

/* =========================================================================
 * PortCls Device Extension Structures & Registration Helper
 * ========================================================================= */

#pragma warning(push)
#pragma warning(disable: 4201) /* nameless struct/union */

typedef struct _SOF_PORTCLS_DC_EXTENSION {
    /* +0x00 */ PVOID   Unknown00;
    /* +0x08 */ PVOID   Unknown08;
    /* +0x10 */ PVOID   Unknown10;
    /* +0x18 */ KEVENT  Event18;   /* NotificationEvent, Signaled = TRUE */
    /* +0x30 */ KEVENT  Event30;   /* SynchronizationEvent, Signaled = FALSE */
    /* +0x48 */ KMUTEX  Mutex48;   /* KeInitializeMutex(&Mutex48, 0) */
} SOF_PORTCLS_DC_EXTENSION, *PSOF_PORTCLS_DC_EXTENSION;

C_ASSERT(offsetof(SOF_PORTCLS_DC_EXTENSION, Event18) == 0x18);
C_ASSERT(offsetof(SOF_PORTCLS_DC_EXTENSION, Event30) == 0x30);
C_ASSERT(offsetof(SOF_PORTCLS_DC_EXTENSION, Mutex48) == 0x48);
C_ASSERT(sizeof(SOF_PORTCLS_DC_EXTENSION) == 0x80);

typedef struct _SOF_PORTCLS_RP_EXTENSION {
    /* +0x00 */ PDEVICE_OBJECT DeviceObject;
    /* +0x08 */ PVOID          Reserved08;
    /* +0x10 */ UCHAR          Flag10;
    /* +0x11 */ UCHAR          Reserved11;
    /* +0x12 */ UCHAR          Reserved12;
    /* +0x13 */ UCHAR          Reserved13;
    /* +0x14 */ UCHAR          Reserved14[0x68 - 0x14];
    /* +0x68 */ KMUTEX         Mutex68;
    /* +0xA0 */ KEVENT         EventA0;
    /* +0xB8 */ KEVENT         EventB8;
    /* +0xD0 */ PVOID          ReservedD0;
    /* +0xD8 */ ULONG          ReservedD8;
    /* +0xDC */ ULONG          ReservedDC;
    /* +0xE0 */ PVOID          ReservedE0;
    /* +0xE8 */ ULONG          FieldE8;
    /* +0xEC */ ULONG          ReservedEC;
    /* +0xF0 */ PVOID          ReservedF0;
    /* +0xF8 */ ULONG          ReservedF8;
    /* +0xFC */ ULONG          ReservedFC;
    /* +0x100*/ PVOID          Reserved100;
    /* +0x108*/ ULONG          Field108;
    /* +0x10C*/ ULONG          Field10C;
    /* +0x110*/ UCHAR          Field110;
    /* +0x111*/ UCHAR          Reserved111[0x120 - 0x111];
} SOF_PORTCLS_RP_EXTENSION, *PSOF_PORTCLS_RP_EXTENSION;

C_ASSERT(offsetof(SOF_PORTCLS_RP_EXTENSION, DeviceObject) == 0x00);
C_ASSERT(offsetof(SOF_PORTCLS_RP_EXTENSION, Mutex68) == 0x68);
C_ASSERT(offsetof(SOF_PORTCLS_RP_EXTENSION, EventA0) == 0xA0);
C_ASSERT(offsetof(SOF_PORTCLS_RP_EXTENSION, EventB8) == 0xB8);
C_ASSERT(offsetof(SOF_PORTCLS_RP_EXTENSION, FieldE8) == 0xE8);
C_ASSERT(offsetof(SOF_PORTCLS_RP_EXTENSION, Field108) == 0x108);
C_ASSERT(sizeof(SOF_PORTCLS_RP_EXTENSION) == 0x120);

typedef struct _SOF_PORTCLS_DEVICE_EXTENSION {
    /* +0x00 */ KSDEVICE_HEADER             KsDeviceHeader;
    /* +0x08 */ PSOF_PORTCLS_DC_EXTENSION   DcExtension;      /* Allocated PcDe (0x80 bytes) */
    /* +0x10 */ PDEVICE_OBJECT              PhysicalDeviceObject;
    /* +0x18 */ PVOID                       StartDevice;
    /* +0x20 */ PVOID                       Reserved20[4];
    /* +0x40 */ PDEVICE_OBJECT              AttachedDeviceObject;
    /* +0x48 */ PVOID                       Subdevices;       /* PKSOBJECT_CREATE_ITEM array (MaxObjects * 0x30 bytes) */
    /* +0x50 */ ULONG                       Magic;            /* 0x000BABEE */
    /* +0x54 */ ULONG                       MaxObjects;       /* e.g. 10 */
    /* +0x58 */ PVOID                       SymbolicLinks;    /* UNICODE_STRING array (MaxObjects * 0x10 bytes) */
    /* +0x60 */ LIST_ENTRY                  SubdeviceList;    /* List of registered subdevice nodes */
    /* +0x70 */ LIST_ENTRY                  PhysicalConnList; /* List of physical connections */
    /* +0x80 */ KMUTEX                      ListMutex;        /* Mutex for subdevice list */
    /* +0xB8 */ KMUTEX                      DeviceMutex;      /* Device Mutex waited by AcquireDevice/ReleaseDevice */
    /* +0xF0 */ PVOID                       ReservedF0[2];
    /* +0x100*/ ULONG                       PowerState;       /* 3 = PowerDeviceD0 */
    /* +0x104*/ ULONG                       PnpState;         /* 4 = Started */
    /* +0x108*/ UCHAR                       Reserved108[2];
    /* +0x10A*/ UCHAR                       BusInterfaceQueried; /* 1 = IoRegisterDeviceInterface */
    /* +0x10B*/ UCHAR                       Flag10B;          /* 1 */
    /* +0x10C*/ ULONG                       Reserved10C[9];
    /* +0x130*/ ULONG                       DevicePowerState; /* 1 = PowerDeviceD0 */
    /* +0x134*/ ULONG                       SystemPowerState; /* 1 = PowerSystemWorking */
    /* +0x138*/ ULONG                       Reserved138[16];
    /* +0x178*/ PIO_REMOVE_LOCK             RemoveLock;       /* Allocated PcLR (0x20 bytes) */
    /* +0x180*/ UCHAR                       Reserved180[16];
    /* +0x190*/ PSOF_PORTCLS_RP_EXTENSION   RpExtension;      /* Allocated PcRP (0x120 bytes) */
    /* +0x198*/ ULONG                       TimerInitialized; /* 1 */
    /* +0x19C*/ ULONG                       Reserved19C;
    /* +0x1A0*/ PVOID                       VerifierTable;    /* NULL */
} SOF_PORTCLS_DEVICE_EXTENSION, *PSOF_PORTCLS_DEVICE_EXTENSION;

#pragma warning(pop)

C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, KsDeviceHeader) == 0x00);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, DcExtension) == 0x08);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, PhysicalDeviceObject) == 0x10);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, AttachedDeviceObject) == 0x40);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, Subdevices) == 0x48);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, Magic) == 0x50);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, MaxObjects) == 0x54);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, SymbolicLinks) == 0x58);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, SubdeviceList) == 0x60);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, PhysicalConnList) == 0x70);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, ListMutex) == 0x80);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, DeviceMutex) == 0xB8);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, PowerState) == 0x100);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, PnpState) == 0x104);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, BusInterfaceQueried) == 0x10A);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, Flag10B) == 0x10B);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, DevicePowerState) == 0x130);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, SystemPowerState) == 0x134);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, RemoveLock) == 0x178);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, RpExtension) == 0x190);
C_ASSERT(offsetof(SOF_PORTCLS_DEVICE_EXTENSION, TimerInitialized) == 0x198);
C_ASSERT(sizeof(SOF_PORTCLS_DEVICE_EXTENSION) == 0x1A8);

extern "C" {
KSDDKAPI
VOID
NTAPI
KsSetDevicePnpAndBaseObject(
    _In_ KSDEVICE_HEADER Header,
    _In_ PDEVICE_OBJECT PnpDeviceObject,
    _In_ PDEVICE_OBJECT BaseDeviceObject
);
}

static void SofFreePortClsDeviceExtension(
    _In_opt_ PDEVICE_OBJECT AudioDevice,
    _In_opt_ PSOF_PORTCLS_DEVICE_EXTENSION Ext
)
{
    if (Ext != NULL) {
        if (Ext->KsDeviceHeader != NULL) {
            KsFreeDeviceHeader(Ext->KsDeviceHeader);
            Ext->KsDeviceHeader = NULL;
        }
        if (Ext->RpExtension != NULL) {
            ExFreePoolWithTag(Ext->RpExtension, 'PcRP');
            Ext->RpExtension = NULL;
        }
        if (Ext->RemoveLock != NULL) {
            ExFreePoolWithTag(Ext->RemoveLock, 'PcLR');
            Ext->RemoveLock = NULL;
        }
        if (Ext->DcExtension != NULL) {
            ExFreePoolWithTag(Ext->DcExtension, 'PcDe');
            Ext->DcExtension = NULL;
        }
        if (Ext->SymbolicLinks != NULL) {
            ExFreePoolWithTag(Ext->SymbolicLinks, 'PclS');
            Ext->SymbolicLinks = NULL;
        }
        if (Ext->Subdevices != NULL) {
            ExFreePoolWithTag(Ext->Subdevices, 'PcCi');
            Ext->Subdevices = NULL;
        }
    }
    if (AudioDevice != NULL) {
        IoDeleteDevice(AudioDevice);
    }
}

extern "C" NTSTATUS NTAPI IoCreateDriver(
    _In_opt_ PUNICODE_STRING DriverName,
    _In_ PDRIVER_INITIALIZE InitializationFunction
);

static PDRIVER_OBJECT g_PortClsDriverObject = NULL;

extern "C" NTSTATUS
SofAudioDriverInit(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = PcDispatchIrp;
    }
    DriverObject->DriverUnload = NULL;
    g_PortClsDriverObject = DriverObject;
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS SofRegisterPortClsSubdevices(
    _In_ WDFDEVICE Device,
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    NTSTATUS status;
    PDEVICE_OBJECT wdmFdo = WdfDeviceWdmGetDeviceObject(Device);
    PDEVICE_OBJECT pdo = WdfDeviceWdmGetPhysicalDevice(Device);
    PDEVICE_OBJECT audioDevice = NULL;
    PPORTWAVERT port = NULL;
    CMiniportWaveRTSof* miniport = NULL;
    PSOF_PORTCLS_DEVICE_EXTENSION ext = NULL;
    const ULONG maxObjects = 10;
    char bmsg[128];

    SOF_VALIDATE_OR_BAIL(wdmFdo != NULL, STATUS_INVALID_PARAMETER, "wdmFdo is NULL");
    SOF_VALIDATE_OR_BAIL(pdo != NULL, STATUS_INVALID_PARAMETER, "pdo is NULL");
    SOF_VALIDATE_OR_BAIL(Ctx != NULL, STATUS_INVALID_PARAMETER, "Ctx is NULL");

    if (g_PortClsDriverObject == NULL) {
        UNICODE_STRING drvName;
        RtlInitUnicodeString(&drvName, L"\\Driver\\SofAudioPortCls");
        NTSTATUS drvStatus = IoCreateDriver(&drvName, SofAudioDriverInit);
        if (!NT_SUCCESS(drvStatus) || g_PortClsDriverObject == NULL) {
            RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[PortCls-1] IoCreateDriver returned 0x%08x (fallback to wdmFdo)\n", drvStatus);
            SofLogBreadcrumb(bmsg);
            g_PortClsDriverObject = wdmFdo->DriverObject;
        } else {
            SofLogBreadcrumb("[PortCls-1] Dedicated \\Driver\\SofAudioPortCls created successfully!\n");
        }
    }

    SofLogBreadcrumb("[PortCls-1] Creating dedicated PortCls audio DEVICE_OBJECT...\n");

    /* 1. Create dedicated PortCls WDM DEVICE_OBJECT under dedicated PortCls DriverObject */
    status = IoCreateDevice(
        g_PortClsDriverObject,
        PORT_CLASS_DEVICE_EXTENSION_SIZE,
        NULL,
        FILE_DEVICE_KS,
        0,
        FALSE,
        &audioDevice
    );
    if (!NT_SUCCESS(status) || audioDevice == NULL) {
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[PortCls-1] IoCreateDevice failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        return status;
    }

    /* 2. Initialize PortCls Device Extension */
    /* Bias Object Manager PointerCount in OBJECT_HEADER (-0x30) to guarantee
     * that ObfDereferenceObject called during file close/cleanup never underflows below 0 (BugCheck 0x18).
     */
    for (ULONG i = 0; i < 100000; i++) {
        ObReferenceObject(audioDevice);
    }
    InterlockedExchange(&audioDevice->ReferenceCount, 100000);
    audioDevice->StackSize = wdmFdo->StackSize;
    audioDevice->Flags |= (DO_DIRECT_IO | DO_POWER_PAGABLE);

    RtlZeroMemory(audioDevice->DeviceExtension, PORT_CLASS_DEVICE_EXTENSION_SIZE);
    ext = (PSOF_PORTCLS_DEVICE_EXTENSION)audioDevice->DeviceExtension;

    ext->Magic = 0x000BABEE;
    ext->MaxObjects = maxObjects;
    ext->PhysicalDeviceObject = pdo;
    ext->AttachedDeviceObject = wdmFdo;

    /* Allocate Subdevices array (KSOBJECT_CREATE_ITEM * maxObjects) */
    ext->Subdevices = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KSOBJECT_CREATE_ITEM) * maxObjects, 'PcCi');
    if (ext->Subdevices == NULL) {
        SofLogBreadcrumb("[PortCls-1] Failed to allocate Subdevices pool!\n");
        SofFreePortClsDeviceExtension(audioDevice, ext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ext->Subdevices, sizeof(KSOBJECT_CREATE_ITEM) * maxObjects);

    /* Allocate SymbolicLinks array (16 bytes * maxObjects) */
    ext->SymbolicLinks = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(UNICODE_STRING) * maxObjects, 'PclS');
    if (ext->SymbolicLinks == NULL) {
        SofLogBreadcrumb("[PortCls-1] Failed to allocate SymbolicLinks pool!\n");
        SofFreePortClsDeviceExtension(audioDevice, ext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ext->SymbolicLinks, sizeof(UNICODE_STRING) * maxObjects);

    /* Allocate and initialize DCExtension (+0x08, 0x80 bytes, tag 'PcDe') */
    ext->DcExtension = (PSOF_PORTCLS_DC_EXTENSION)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(SOF_PORTCLS_DC_EXTENSION), 'PcDe');
    if (ext->DcExtension == NULL) {
        SofLogBreadcrumb("[PortCls-1] Failed to allocate DcExtension pool!\n");
        SofFreePortClsDeviceExtension(audioDevice, ext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ext->DcExtension, sizeof(SOF_PORTCLS_DC_EXTENSION));
    KeInitializeEvent(&ext->DcExtension->Event18, NotificationEvent, TRUE);
    KeInitializeEvent(&ext->DcExtension->Event30, SynchronizationEvent, FALSE);
    KeInitializeMutex(&ext->DcExtension->Mutex48, 0);

    /* NOTE: PortCls AcquireRemoveLock / ReleaseRemoveLock check if (+0x178) is NULL,
     * and if NULL, safely return STATUS_SUCCESS without calling IoReleaseRemoveLockEx.
     * KMDF already manages remove locks for the entire device stack. Leaving RemoveLock NULL
     * prevents IoReleaseRemoveLockEx from signaling an uninitialized/mismatched RemoveEvent under Driver Verifier.
     */
    ext->RemoveLock = NULL;

    /* Allocate and initialize RpExtension (+0x190, 0x120 bytes, tag 'PcRP') */
    ext->RpExtension = (PSOF_PORTCLS_RP_EXTENSION)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(SOF_PORTCLS_RP_EXTENSION), 'PcRP');
    if (ext->RpExtension != NULL) {
        RtlZeroMemory(ext->RpExtension, sizeof(SOF_PORTCLS_RP_EXTENSION));
        ext->RpExtension->DeviceObject = audioDevice;
        ext->RpExtension->Flag10 = 1;
        KeInitializeMutex(&ext->RpExtension->Mutex68, 0);
        KeInitializeEvent(&ext->RpExtension->EventA0, NotificationEvent, TRUE);
        KeInitializeEvent(&ext->RpExtension->EventB8, NotificationEvent, TRUE);
        ext->RpExtension->FieldE8 = 2;
        ext->RpExtension->Field108 = 1;
    }

    InitializeListHead(&ext->SubdeviceList);
    InitializeListHead(&ext->PhysicalConnList);

    KeInitializeMutex(&ext->ListMutex, 0);
    KeInitializeMutex(&ext->DeviceMutex, 0);

    ext->PowerState = 3;          /* System power state working */
    ext->PnpState = 3;            /* Started (3 in PortCls!) */
    ext->DevicePowerState = 1;    /* PowerDeviceD0 (1) */
    ext->SystemPowerState = 1;    /* PowerSystemWorking (1) */
    ext->BusInterfaceQueried = 1;  /* Enables IoRegisterDeviceInterface on PDO */
    ext->Flag10B = 1;
    ext->TimerInitialized = 1;

    /* Initialize KsDeviceHeader with Subdevices create item list */
    status = KsAllocateDeviceHeader(&ext->KsDeviceHeader, maxObjects, (PKSOBJECT_CREATE_ITEM)ext->Subdevices);
    if (!NT_SUCCESS(status)) {
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[PortCls-1] KsAllocateDeviceHeader failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        SofFreePortClsDeviceExtension(audioDevice, ext);
        return status;
    }

    /* Bind KS device header to Pnp and Base Device Objects */
    KsSetDevicePnpAndBaseObject(ext->KsDeviceHeader, wdmFdo, pdo);

    audioDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Save audioDevice in context */
    Ctx->AudioPortClsDevice = audioDevice;

    RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[PortCls-1] Audio DEVICE_OBJECT created (dev=%p, ext=%p, dc=%p)\n",
                       audioDevice, ext, ext->DcExtension);
    SofLogBreadcrumb(bmsg);

    /* 3. Create PortCls WaveRT Port */
    SofLogBreadcrumb("[PortCls-2] Calling PcNewPort(CLSID_PortWaveRT)...\n");
    status = PcNewPort((PPORT*)&port, CLSID_PortWaveRT);
    RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[PortCls-2] PcNewPort returned 0x%08x (port=%p)\n", status, port);
    SofLogBreadcrumb(bmsg);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] PcNewPort(CLSID_PortWaveRT) failed: 0x%08x\n", status);
        SofFreePortClsDeviceExtension(audioDevice, ext);
        Ctx->AudioPortClsDevice = NULL;
        return status;
    }

    /* 4. Create SOF WaveRT Miniport */
    SofLogBreadcrumb("[PortCls-3] Instantiating CMiniportWaveRTSof...\n");
    miniport = new (POOL_FLAG_NON_PAGED, SOF_DEVICE_TAG) CMiniportWaveRTSof(Ctx);
    if (miniport == NULL) {
        SofLogBreadcrumb("[PortCls-3] CMiniportWaveRTSof allocation failed!\n");
        port->Release();
        SofFreePortClsDeviceExtension(audioDevice, ext);
        Ctx->AudioPortClsDevice = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    SofLogBreadcrumb("[PortCls-3] CMiniportWaveRTSof instantiated successfully.\n");

    /* 5. Initialize Port with Miniport and Audio Device Object */
    SofLogBreadcrumb("[PortCls-4] Calling port->Init with audioDevice...\n");
    status = port->Init(audioDevice, NULL, miniport, NULL, NULL);
    RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[PortCls-4] port->Init returned 0x%08x\n", status);
    SofLogBreadcrumb(bmsg);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Port->Init failed: 0x%08x\n", status);
        miniport->Release();
        port->Release();
        SofFreePortClsDeviceExtension(audioDevice, ext);
        Ctx->AudioPortClsDevice = NULL;
        return status;
    }

    /* 6. Register Subdevice with PortCls (exposing Wave endpoints) */
    SofLogBreadcrumb("[PortCls-5] Calling PcRegisterSubdevice(audioDevice, L\"Wave\", port)...\n");
    status = PcRegisterSubdevice(audioDevice, L"Wave", port);
    RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[PortCls-5] PcRegisterSubdevice returned 0x%08x\n", status);
    SofLogBreadcrumb(bmsg);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] PcRegisterSubdevice(L\"Wave\") failed: 0x%08x\n", status);
        miniport->Release();
        port->Release();
        SofFreePortClsDeviceExtension(audioDevice, ext);
        Ctx->AudioPortClsDevice = NULL;
        return status;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Successfully registered PortCls WaveRT subdevice.\n");

    /* Retain port reference in device context for clean unregistration on shutdown */
    Ctx->WavePort = (PVOID)port;
    Ctx->PortClsRegistered = TRUE;

    /* Release miniport local reference (port holds a reference) */
    miniport->Release();

    return status;
}

/**
 * @brief Registers the PortCls WaveRT subdevice with Windows audio architecture.
 *
 * @param[in] Device WDFDEVICE handle.
 * @param[in] Ctx    Pointer to SOF device context.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
extern "C" NTSTATUS SofRegisterPortClsSubdevices(
    _In_ WDFDEVICE Device,
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Unregisters the PortCls WaveRT subdevice on driver teardown.
 *
 * @param[in] Device WDFDEVICE handle.
 * @param[in] Ctx    Pointer to SOF device context.
 * @return STATUS_SUCCESS.
 */
extern "C" NTSTATUS SofUnregisterPortClsSubdevices(
    _In_ WDFDEVICE Device,
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    UNREFERENCED_PARAMETER(Device);
    PPORTWAVERT port = NULL;
    PDEVICE_OBJECT audioDevice = NULL;
    PSOF_PORTCLS_DEVICE_EXTENSION ext = NULL;

    if (Ctx == NULL || !Ctx->PortClsRegistered || Ctx->WavePort == NULL) {
        return STATUS_SUCCESS;
    }

    port = (PPORTWAVERT)Ctx->WavePort;
    audioDevice = Ctx->AudioPortClsDevice;
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Unregistering PortCls WaveRT subdevice...\n");

    if (audioDevice != NULL) {
        ext = (PSOF_PORTCLS_DEVICE_EXTENSION)audioDevice->DeviceExtension;
    }

    IUnregisterSubdevice* unreg = NULL;
    NTSTATUS status = port->QueryInterface(IID_IUnregisterSubdevice, (PVOID*)&unreg);
    if (NT_SUCCESS(status) && unreg != NULL) {
        unreg->UnregisterSubdevice(audioDevice, port);
        unreg->Release();
    }

    port->Release();
    Ctx->WavePort = NULL;
    Ctx->PortClsRegistered = FALSE;

    SofFreePortClsDeviceExtension(audioDevice, ext);
    Ctx->AudioPortClsDevice = NULL;

    return STATUS_SUCCESS;
}

/**
 * @brief Signals KSEVENT_PINCAPS_JACKINFOCHANGE to notify Windows of jack insertion/removal.
 *
 * @param[in] Ctx   Pointer to SOF device context.
 * @param[in] PinId Hardware pin index that experienced the event.
 */
extern "C" void SofPortClsNotifyJackPresence(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PinId
)
{
    if (!Ctx || !Ctx->PortEvents) {
        return;
    }

    PPORTEVENTS portEvents = (PPORTEVENTS)Ctx->PortEvents;
    GUID eventSet = KSEVENTSETID_PinCapsChange;

    portEvents->GenerateEventList(
        &eventSet,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        TRUE,   /* PinEvent */
        PinId,  /* PinId */
        FALSE,  /* NodeEvent */
        0       /* NodeId */
    );

    char msg[128];
    RtlStringCbPrintfA(msg, sizeof(msg),
        "[PortCls] GenerateEventList KSEVENT_PINCAPS_JACKINFOCHANGE fired for Pin %u\n", PinId);
    SofLogBreadcrumb(msg);
}

/**
 * @brief Notifies Windows of USB Audio Offload device arrival (pins 17 & 19).
 *
 * @param[in] Ctx Pointer to SOF device context.
 */
extern "C" void SofPortClsNotifyUsbOffloadArrival(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    SofLogBreadcrumb("[PortCls] USB Offload Device Arrival -> Notifying Pins 17 & 19\n");
    SofPortClsNotifyJackPresence(Ctx, 17);
    SofPortClsNotifyJackPresence(Ctx, 19);
}

/**
 * @brief Notifies Windows of USB Audio Offload device removal (pins 17 & 19).
 *
 * @param[in] Ctx Pointer to SOF device context.
 */
extern "C" void SofPortClsNotifyUsbOffloadRemoval(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    SofLogBreadcrumb("[PortCls] USB Offload Device Removal -> Notifying Pins 17 & 19\n");
    SofPortClsNotifyJackPresence(Ctx, 17);
    SofPortClsNotifyJackPresence(Ctx, 19);
}

#endif /* _KERNEL_MODE */
