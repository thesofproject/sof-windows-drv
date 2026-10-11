/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file HdaVerb.h
 * @brief High Definition Audio (HD-A) Immediate Command Verb Engine & Codec Probe.
 *
 * @details
 * Declares register definitions, standard HD-A verb IDs, helper packing macros,
 * and API functions for transmitting immediate verbs across Intel HD-A BAR0 (Immediate
 * Command Output / Input Interface), probing and initializing audio codecs (e.g. Realtek ALC888/285),
 * configuring DAC/ADC converters, and discovering Intel Display Audio (iDISP) codecs on CAD 2
 * including EDID-Like Data (ELD) parsing.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_HDA_VERB_H__
#define __SOF_HDA_VERB_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#include <windows.h>
#endif
#include "Device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Immediate Command Registers (BAR0) */
#define SOF_HDA_REG_ICOI                0x60    /**< Immediate Command Output Interface (32-bit verb write) */
#define SOF_HDA_REG_ICII                0x64    /**< Immediate Response Input Interface (32-bit response read) */
#define SOF_HDA_REG_ICS                 0x68    /**< Immediate Command Status (16-bit status and trigger) */

/* Immediate Command Status (ICS) Bits */
#define SOF_HDA_ICS_ICB                 0x0001  /**< Immediate Command Busy: Set to 1 to transmit; cleared by HW when done */
#define SOF_HDA_ICS_IRV                 0x0002  /**< Immediate Result Valid: Set by HW when response is in ICII; clear by writing 1 */
#define SOF_HDA_ICS_ICIE                0x0004  /**< Immediate Command Interrupt Enable */

/* Standard 12-Bit HD-A Verbs */
#define HDA_VERB_GET_PARAM              0xF00   /**< Get Parameter verb */
#define HDA_PARAM_VENDOR_ID             0x00    /**< Parameter: Vendor & Device ID */
#define HDA_PARAM_REV_ID                0x02    /**< Parameter: Revision ID */
#define HDA_PARAM_SUB_NODE_COUNT        0x04    /**< Parameter: Subordinate Node Count */
#define HDA_PARAM_FUNCTION_TYPE         0x05    /**< Parameter: Function Group Type */
#define HDA_PARAM_AUDIO_WIDGET_CAP      0x09    /**< Parameter: Audio Widget Capabilities */

#define HDA_VERB_GET_CONN_SELECT        0xF01   /**< Get Connection Select */
#define HDA_VERB_GET_CONN_LIST          0xF02   /**< Get Connection List Entry */
#define HDA_VERB_GET_POWER_STATE        0xF05   /**< Get Power State (D0..D3) */
#define HDA_VERB_GET_CONV_STREAM_CHAN   0xF06   /**< Get Converter Stream & Channel */
#define HDA_VERB_GET_PIN_WIDGET_CTRL    0xF07   /**< Get Pin Widget Control */
#define HDA_VERB_GET_PIN_SENSE          0xF09   /**< Get Pin Sense (jack presence & impedance) */
#define HDA_PIN_SENSE_PRESENCE          0x80000000 /**< Pin Sense: Presence Detect (bit 31) */
#define HDA_PIN_SENSE_ELD_VALID         0x40000000 /**< Pin Sense: ELD Valid (bit 30) */
#define HDA_VERB_GET_HDMI_ELD           0xF2F   /**< Get HDMI/DisplayPort ELD Data Byte */

#define HDA_VERB_SET_CONN_SELECT        0x701   /**< Set Connection Select */
#define HDA_VERB_SET_POWER_STATE        0x705   /**< Set Power State (0=D0, 3=D3hot) */
#define HDA_VERB_SET_CONV_STREAM_CHAN   0x706   /**< Set Converter Stream & Channel */
#define HDA_VERB_SET_PIN_WIDGET_CTRL    0x707   /**< Set Pin Widget Control (VRef, OutEn, InEn) */
#define HDA_VERB_SET_EAPD_BTL           0x70C   /**< Set External Amplifier Power Down (EAPD) */

/* Codec Address (CAD) Indices */
#define HDA_CAD_CODEC                   0       /**< Primary Audio Codec (CAD 0) */
#define HDA_CAD_IDISP                   2       /**< Intel Integrated Display Audio Codec (CAD 2) */

/* 4-Bit Verbs (Verb ID in bits 19:16, 16-bit payload in bits 15:0) */
#define HDA_VERB_GET_CONV_FMT           0xA     /**< Get Converter Format (4-bit verb) */
#define HDA_VERB_SET_CONV_FMT           0x2     /**< Set Converter Format (4-bit verb) */
#define HDA_VERB_SET_AMP_GAIN_MUTE      0x3     /**< Set Amplifier Gain / Mute (4-bit verb) */

/**
 * @def HDA_VERB
 * @brief Constructs a 32-bit HD-A command verb (12-bit verb format).
 * @param cad   Codec Address (4 bits, 28..31).
 * @param nid   Node ID (8 bits, 20..27).
 * @param verb  12-bit Verb ID (12 bits, 8..19).
 * @param param 8-bit Parameter (8 bits, 0..7).
 */
#define HDA_VERB(cad, nid, verb, param) \
    (((ULONG)(cad) << 28) | ((ULONG)(nid) << 20) | ((ULONG)(verb) << 8) | ((ULONG)(param) & 0xFF))

/**
 * @def HDA_VERB_4BIT
 * @brief Constructs a 32-bit HD-A command verb (4-bit verb format).
 * @param cad     Codec Address (4 bits, 28..31).
 * @param nid     Node ID (8 bits, 20..27).
 * @param verb4   4-bit Verb ID (4 bits, 16..19).
 * @param param16 16-bit Parameter payload (16 bits, 0..15).
 */
#define HDA_VERB_4BIT(cad, nid, verb4, param16) \
    (((ULONG)(cad) << 28) | ((ULONG)(nid) << 20) | (((ULONG)(verb4) & 0x0F) << 16) | ((ULONG)(param16) & 0xFFFF))

/**
 * @brief Transmits a single HD-A command verb via BAR0 Immediate Command registers.
 *
 * Sequence:
 *  1. Checks target Codec Address (CAD) presence in WAKESTS.
 *  2. Waits for Immediate Command Busy (ICB) to clear. If stuck, cycles GCTL.CRST.
 *  3. Clears Immediate Result Valid (IRV).
 *  4. Writes verb to ICOI.
 *  5. Sets ICB in ICS to initiate hardware command transmission.
 *  6. Polls for ICB to clear and IRV to assert.
 *  7. Reads response from ICII and clears IRV.
 *
 * @param[in]  Ctx      Pointer to SOF device context.
 * @param[in]  Verb     Packed 32-bit HD-A verb.
 * @param[out] Response Optional pointer to receive 32-bit codec response.
 * @return STATUS_SUCCESS on success; STATUS_IO_TIMEOUT on timeout; STATUS_DEVICE_NOT_CONNECTED if CAD asleep.
 */
NTSTATUS SofHdaSendVerb(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG Verb,
    _Out_opt_ PULONG Response
);

/**
 * @brief Probes an attached HD-A codec, reading Vendor ID and Revision ID.
 *
 * @param[in]  Ctx          Pointer to SOF device context.
 * @param[in]  CodecAddress Codec address (CAD 0..14).
 * @param[out] VendorId     Receives 32-bit Vendor/Device ID (e.g. 0x10EC0285).
 * @param[out] RevisionId   Receives 32-bit Revision ID.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofHdaProbeCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _Out_ PULONG VendorId,
    _Out_ PULONG RevisionId
);

/**
 * @brief Sends initialization verbs to power on and unmute Realtek ALC888 / ALC285.
 *
 * @param[in] Ctx          Pointer to SOF device context.
 * @param[in] CodecAddress Codec address (CAD 0..14).
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofHdaInitCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress
);

/**
 * @brief Configures codec converter (DAC or ADC) for streaming with specified Stream Tag.
 *
 * @param[in] Ctx          Pointer to SOF device context.
 * @param[in] CodecAddress Codec address (CAD 0..14).
 * @param[in] IsCapture    TRUE for capture/ADC; FALSE for playback/DAC.
 * @param[in] StreamTag    HD-A stream tag (1..15).
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofHdaStartConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ BOOLEAN IsCapture,
    _In_ ULONG StreamTag
);

/**
 * @brief Halts codec converter (DAC or ADC) by resetting its stream tag to 0.
 *
 * @param[in] Ctx          Pointer to SOF device context.
 * @param[in] CodecAddress Codec address (CAD 0..14).
 * @param[in] IsCapture    TRUE for capture; FALSE for playback.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofHdaStopConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ BOOLEAN IsCapture
);

/**
 * @brief Probes Intel Integrated Display Audio (iDISP) codec on CAD 2.
 *
 * @param[in]  Ctx          Pointer to SOF device context.
 * @param[in]  CodecAddress Codec address (typically HDA_CAD_IDISP = 2).
 * @param[out] VendorId     Receives 32-bit Vendor/Device ID (0x808628xx).
 * @param[out] RevisionId   Receives 32-bit Revision ID.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofHdaProbeDisplayCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _Out_ PULONG VendorId,
    _Out_ PULONG RevisionId
);

/**
 * @brief Initializes display audio codec pin widgets and converters on CAD 2.
 *
 * @param[in] Ctx          Pointer to SOF device context.
 * @param[in] CodecAddress Codec address (typically HDA_CAD_IDISP = 2).
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofHdaInitDisplayCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress
);

/**
 * @brief Computes standard 16-bit HD-A stream/converter format word from audio parameters.
 *
 * Format layout:
 *  - Bit 15: Type (0: PCM, 1: Non-PCM)
 *  - Bit 14: Sample base rate (0: 48 kHz, 1: 44.1 kHz)
 *  - Bits 13:11: Sample base rate mult (000: x1, 001: x2, etc.)
 *  - Bits 10:8: Sample base rate div (000: /1, 001: /2, etc.)
 *  - Bits 6:4: Bits per sample (001: 16-bit, 010: 20-bit, 011: 24-bit, 100: 32-bit)
 *  - Bits 3:0: Channels (0: 1ch, 1: 2ch, ..., 15: 16ch)
 *
 * @param[in] SampleRate    Sampling frequency in Hz (e.g. 48000, 96000).
 * @param[in] BitsPerSample Bit depth (16, 24, 32).
 * @param[in] Channels      Channel count (1..16).
 * @return 16-bit packed HD-A format word.
 */
USHORT SofHdaComputeFormatWord(
    _In_ ULONG SampleRate,
    _In_ ULONG BitsPerSample,
    _In_ ULONG Channels
);

/**
 * @brief Configures and starts display audio converter for specified HDMI link.
 *
 * @param[in]     Ctx          Pointer to SOF device context.
 * @param[in]     CodecAddress Codec address (CAD 2).
 * @param[in]     LinkIndex    HDMI link index (1, 2, or 3).
 * @param[in]     StreamTag    Stream tag to assign (1..15).
 * @param[in,opt] Format       Audio format specification; if NULL, defaults to 48kHz 16-bit stereo.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofHdaStartDisplayConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG LinkIndex,
    _In_ ULONG StreamTag,
    _In_opt_ const struct _SOF_AUDIO_FORMAT_PARAMS *Format
);

/**
 * @brief Stops display audio converter for specified HDMI link.
 *
 * @param[in] Ctx          Pointer to SOF device context.
 * @param[in] CodecAddress Codec address (CAD 2).
 * @param[in] LinkIndex    HDMI link index (1, 2, or 3).
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofHdaStopDisplayConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG LinkIndex
);

/**
 * @brief Queries display pin sense to detect monitor connection and ELD validity.
 *
 * @param[in]  Ctx          Pointer to SOF device context.
 * @param[in]  CodecAddress Codec address (CAD 2).
 * @param[in]  PinNid       Pin widget Node ID.
 * @param[out] Connected    Receives TRUE if monitor is physically connected.
 * @param[out] EldValid     Receives TRUE if EDID-Like Data (ELD) buffer is valid.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofHdaGetDisplayPinSense(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG PinNid,
    _Out_ PBOOLEAN Connected,
    _Out_ PBOOLEAN EldValid
);

/**
 * @brief Reads the raw EDID-Like Data (ELD) byte buffer from a display pin widget.
 *
 * @param[in]  Ctx          Pointer to SOF device context.
 * @param[in]  CodecAddress Codec address (CAD 2).
 * @param[in]  PinNid       Pin widget Node ID.
 * @param[out] EldBuffer    Caller-allocated buffer to receive ELD bytes.
 * @param[in]  MaxLen       Maximum capacity of EldBuffer.
 * @param[out] ActualLen    Receives actual number of bytes read.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofHdaGetDisplayEld(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG PinNid,
    _Out_writes_to_(MaxLen, *ActualLen) PUCHAR EldBuffer,
    _In_ ULONG MaxLen,
    _Out_ PULONG ActualLen
);

/**
 * @brief Extracts human-readable monitor name from baseline ELD buffer.
 *
 * @param[in]  EldBuffer Baseline ELD byte array.
 * @param[in]  EldLen    Length of ELD buffer.
 * @param[out] NameOut   Wide-character string buffer receiving monitor name.
 * @param[in]  MaxChars  Maximum character capacity of NameOut.
 */
VOID SofHdaParseMonitorName(
    _In_reads_(EldLen) const UCHAR *EldBuffer,
    _In_ ULONG EldLen,
    _Out_writes_(MaxChars) PWCHAR NameOut,
    _In_ ULONG MaxChars
);

/**
 * @brief Parses CEA-861 Short Audio Descriptors (SADs) from ELD buffer.
 *
 * @param[in]  EldBuffer        Baseline ELD byte array.
 * @param[in]  EldLen           Length of ELD buffer.
 * @param[out] SampleRatesMask  Receives bitmask of supported sample rates.
 * @param[out] BitDepthsMask    Receives bitmask of supported bit depths.
 * @param[out] MaxChannels      Receives maximum number of supported audio channels.
 */
VOID SofHdaParseEldAudioCapabilities(
    _In_reads_(EldLen) const UCHAR *EldBuffer,
    _In_ ULONG EldLen,
    _Out_ PULONG SampleRatesMask,
    _Out_ PULONG BitDepthsMask,
    _Out_ PULONG MaxChannels
);

/**
 * @brief Polls jack presence state on active audio pins and notifies PortCls on changes.
 *
 * @param[in] Ctx Pointer to SOF device context.
 */
VOID SofCheckAndNotifyJackPresence(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

#ifdef __cplusplus
}
#endif

#endif /* __SOF_HDA_VERB_H__ */
