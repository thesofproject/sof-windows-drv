/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file HdaVerb.c
 * @brief High Definition Audio (HD-A) Immediate Command Verb Engine Implementation.
 *
 * @details
 * Implements low-level register access, immediate command transmission, codec power
 * state management, converter/pin routing, and Display Audio (iDISP) ELD discovery
 * across Intel HD-A controllers (BAR0).
 *
 * Hardware Mechanisms:
 *  - Immediate Command Interface: ICOI (0x60), ICII (0x64), ICS (0x68).
 *  - Stuck Command Recovery: Automatic GCTL.CRST reset toggling to clear hung codecs.
 *  - Codec CAD Detection: Verifies WAKESTS before issuing transactions.
 *  - Analog Codec Path: Realtek ALC888/285 DAC/ADC initialization, EAPD power-up.
 *  - Display Audio Path: Intel iDISP CAD 2 probe, converter setup, ELD parsing, and
 *    jack sense change propagation to Windows PortCls.
 * Copyright 2026 The SOF Authors
 */

#ifdef _KERNEL_MODE

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "HdaVerb.h"
#include "PortClsAdapter.h"

void SofLogBreadcrumb(const char* text);

/**
 * @brief Transmits a 32-bit HD-A command verb via BAR0 Immediate Command registers.
 *
 * @param[in]  Ctx      Pointer to SOF device context.
 * @param[in]  Verb     Packed 32-bit HD-A verb.
 * @param[out] Response Optional pointer to receive 32-bit codec response.
 * @return STATUS_SUCCESS on success; STATUS_IO_TIMEOUT on timeout;
 *         STATUS_DEVICE_NOT_CONNECTED if target CAD is asleep.
 */
NTSTATUS SofHdaSendVerb(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG Verb,
    _Out_opt_ PULONG Response
)
{
    PUCHAR bar0;
    USHORT ics;
    ULONG timeout;

    KLOCK_QUEUE_HANDLE lockHandle;

    if (!Ctx || !Ctx->Bar0VirtualAddress) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->CurrentPowerState != PowerDeviceD0) {
        return STATUS_DEVICE_NOT_READY;
    }
    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;

    KeAcquireInStackQueuedSpinLock(&Ctx->HdaVerbLock, &lockHandle);

    /* 0. Verify the target Codec Address (CAD) is awake in WAKESTS */
    {
        ULONG cad = (Verb >> 28) & 0x0F;
        if (cad < 15) {
            USHORT wakests = readw(bar0 + SOF_HDA_WAKESTS);
            if (wakests == 0xFFFF || !(wakests & (1 << cad))) {
                KeReleaseInStackQueuedSpinLock(&lockHandle);
                return STATUS_DEVICE_NOT_CONNECTED;
            }
        }
    }

    /* 1. Ensure ICB is 0 before writing new command */
    timeout = 10000;
    while (((ics = readw(bar0 + SOF_HDA_REG_ICS)) & SOF_HDA_ICS_ICB) && --timeout) {
        if (ics == 0xFFFF) {
            KeReleaseInStackQueuedSpinLock(&lockHandle);
            return STATUS_DEVICE_NOT_READY;
        }
        KeStallExecutionProcessor(5);
    }
    if (timeout == 0) {
        /* Recovery: If ICB is stuck from a previous transaction, cycle GCTL.CRST to reset the command engine */
        ULONG gctl = readl(bar0 + SOF_HDA_GCTL);
        writel(gctl & ~SOF_HDA_GCTL_RESET, bar0 + SOF_HDA_GCTL);
        KeStallExecutionProcessor(200);
        writel(gctl | SOF_HDA_GCTL_RESET, bar0 + SOF_HDA_GCTL);
        KeStallExecutionProcessor(1000);

        ics = readw(bar0 + SOF_HDA_REG_ICS);
        if (ics & SOF_HDA_ICS_ICB) {
            KeReleaseInStackQueuedSpinLock(&lockHandle);
            char msg[128];
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[HDA-Verb] Timeout waiting for ICB=0 prior to write (ICS=0x%04X)\n", ics);
            SofLogBreadcrumb(msg);
            return STATUS_IO_TIMEOUT;
        }
        SofLogBreadcrumb("[HDA-Verb] Cleared stuck ICB via GCTL.CRST prior to write\n");
    }

    /* 2. Clear IRV if previously set */
    ics = readw(bar0 + SOF_HDA_REG_ICS);
    if (ics & SOF_HDA_ICS_IRV) {
        writew(SOF_HDA_ICS_IRV, bar0 + SOF_HDA_REG_ICS);
    }

    /* 3. Write verb to ICOI */
    writel(Verb, bar0 + SOF_HDA_REG_ICOI);

    /* 4. Set ICB (bit 0) to trigger transmission */
    writew(SOF_HDA_ICS_ICB, bar0 + SOF_HDA_REG_ICS);

    /* 5. Wait for ICB to clear */
    timeout = 10000;
    while (((ics = readw(bar0 + SOF_HDA_REG_ICS)) & SOF_HDA_ICS_ICB) && --timeout) {
        KeStallExecutionProcessor(5);
    }
    if (timeout == 0) {
        /* Recovery: Cycle GCTL.CRST to clear stuck command so future verbs are never blocked */
        ULONG gctl = readl(bar0 + SOF_HDA_GCTL);
        writel(gctl & ~SOF_HDA_GCTL_RESET, bar0 + SOF_HDA_GCTL);
        KeStallExecutionProcessor(200);
        writel(gctl | SOF_HDA_GCTL_RESET, bar0 + SOF_HDA_GCTL);
        KeStallExecutionProcessor(1000);

        KeReleaseInStackQueuedSpinLock(&lockHandle);
        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[HDA-Verb] Timeout executing verb 0x%08X (ICS=0x%04X, cleared via CRST)\n", Verb, ics);
        SofLogBreadcrumb(msg);
        return STATUS_IO_TIMEOUT;
    }

    /* 6. Check IRV (bit 1) */
    if (ics & SOF_HDA_ICS_IRV) {
        ULONG resp = readl(bar0 + SOF_HDA_REG_ICII);
        if (Response) {
            *Response = resp;
        }
        /* Clear IRV by writing 1 to it */
        writew(SOF_HDA_ICS_IRV, bar0 + SOF_HDA_REG_ICS);
        KeReleaseInStackQueuedSpinLock(&lockHandle);
        return STATUS_SUCCESS;
    }

    KeReleaseInStackQueuedSpinLock(&lockHandle);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS SofHdaProbeCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _Out_ PULONG VendorId,
    _Out_ PULONG RevisionId
)
{
    NTSTATUS status;
    ULONG vid = 0;
    ULONG rev = 0;
    ULONG verb;

    if (!VendorId || !RevisionId) return STATUS_INVALID_PARAMETER;

    /* Verb: Get Parameter (0xF00), Param 0x00 = Vendor ID */
    verb = (CodecAddress << 28) | (0 << 20) | (HDA_VERB_GET_PARAM << 8) | HDA_PARAM_VENDOR_ID;
    status = SofHdaSendVerb(Ctx, verb, &vid);
    if (!NT_SUCCESS(status) || vid == 0 || vid == 0xFFFFFFFF) {
        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[HDA-Probe] Codec CAD=%u probe failed: status=0x%08x, vid=0x%08x\n",
            CodecAddress, status, vid);
        SofLogBreadcrumb(msg);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    /* Verb: Get Parameter (0xF00), Param 0x02 = Revision ID */
    verb = (CodecAddress << 28) | (0 << 20) | (HDA_VERB_GET_PARAM << 8) | HDA_PARAM_REV_ID;
    status = SofHdaSendVerb(Ctx, verb, &rev);
    if (!NT_SUCCESS(status)) {
        rev = 0;
    }

    *VendorId = vid;
    *RevisionId = rev;

    char pmsg[128];
    RtlStringCbPrintfA(pmsg, sizeof(pmsg),
        "[HDA-Probe] Codec CAD=%u detected: VendorID=0x%08X, RevID=0x%08X\n",
        CodecAddress, vid, rev);
    SofLogBreadcrumb(pmsg);

    return STATUS_SUCCESS;
}

NTSTATUS SofHdaInitCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress
)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG dummy = 0;
    ULONG i;
    char msg[128];

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-Init] Initializing Codec CAD=%u (VendorID=0x%08X)...\n",
        CodecAddress, Ctx->CodecVendorId);
    SofLogBreadcrumb(msg);

    /* 1. AFG (Node 0x01): Power State D0 */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x01, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);

    /* 2. DAC 1 (Node 0x02 - Front/Speakers): Power D0, 48kHz/16-bit/2ch format, Unmute 0dB */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x02, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x02, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x02, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB000 | 0x00), &dummy);

    /* 3. DAC 2 (Node 0x03 - Headphone): Power D0, 48kHz/16-bit/2ch format, Unmute 0dB */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x03, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x03, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x03, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB000 | 0x00), &dummy);

    /* 4. Output Mixer 0x0C (Front Sum): Power D0, Unmute DAC input (idx 0), Unmute Output */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x0C, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x0C, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (0 << 8) | 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x0C, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB000 | 0x00), &dummy);

    /* 5. Pin 0x14 (Rear Line Out - Green): Power D0, Pin Out (0x40), EAPD high (0x02), Unmute */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x14, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x14, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x40), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x14, HDA_VERB_SET_EAPD_BTL, 0x02), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x14, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB000 | 0x00), &dummy);

    /* 6. Pin 0x1B (Front Headphone Out): Power D0, Pin Out + HP Amp (0xC0), EAPD high (0x02), Unmute */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x1B, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x1B, HDA_VERB_SET_PIN_WIDGET_CTRL, 0xC0), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x1B, HDA_VERB_SET_EAPD_BTL, 0x02), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x1B, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB000 | 0x00), &dummy);

    /* 7. Pin 0x21 (ALC285 Headphone Out compatibility): Power D0, Pin Out + HP Amp (0xC0), Unmute */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x21, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x21, HDA_VERB_SET_PIN_WIDGET_CTRL, 0xC0), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x21, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB000 | 0x00), &dummy);

    /* --- Capture Path Initialization --- */

    /* 8. Input Pin 0x18 (Rear Mic 1 - Pink): Power D0, Pin In + VRef 80% (0x24), Boost +30dB */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x18, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x18, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x24), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x18, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (0 << 8) | 0x03), &dummy);

    /* 9. Input Pin 0x19 (Front Mic 2 - Pink / ALC285 Ext Mic): Power D0, Pin In + VRef 80% (0x24), Boost +30dB */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x19, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x19, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x24), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x19, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (0 << 8) | 0x03), &dummy);

    /* 10. Input Pin 0x1A (Rear Line In - Blue): Power D0, Pin In (0x20), Boost 0dB */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x1A, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x1A, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x20), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x1A, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (0 << 8) | 0x00), &dummy);

    /* 11. Pin 0x12 (ALC285 DMIC compatibility): Power D0, Pin In (0x20) */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x12, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x12, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x20), &dummy);

    /* 12. Summing Mixer Node 0x0B (Stereo Mix / Analog Loopback):
     * Power D0, Unmute all 8 inputs (DAC 0x02/0x03, Mics, Line In)
     */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x0B, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    for (i = 0; i < 8; i++) {
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x0B, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (i << 8) | 0x00), &dummy);
    }

    /* 13. Input Selector MUX Node 0x23 (Routes into ADC 1 Node 0x08):
     * Power D0, Select Index 6 (Node 0x0B Summing Mixer / Stereo Mix loopback), Unmute all 8 inputs
     */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x23, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x23, HDA_VERB_SET_CONN_SELECT, 0x06), &dummy);
    for (i = 0; i < 8; i++) {
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x23, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (i << 8) | 0x00), &dummy);
    }

    /* 14. Input Selector MUX Node 0x24 (Routes into ADC 2 Node 0x09):
     * Power D0, Select Index 0 (Pin 0x18 Mic 1), Unmute all 8 inputs
     */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x24, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x24, HDA_VERB_SET_CONN_SELECT, 0x00), &dummy);
    for (i = 0; i < 8; i++) {
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x24, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (i << 8) | 0x00), &dummy);
    }

    /* 15. ADC 1 (Node 0x08 - Capture Converter 1):
     * Power D0, 48kHz/16-bit/2ch format, Input Amp Unmute + Max Gain (0x1F = +30dB)
     */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x08, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x08, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x08, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (0 << 8) | 0x1F), &dummy);

    /* 16. ADC 2 (Node 0x09 - Capture Converter 2):
     * Power D0, 48kHz/16-bit/2ch format, Input Amp Unmute + Max Gain (0x1F = +30dB)
     */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x09, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x09, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x09, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (0 << 8) | 0x1F), &dummy);

    /* 17. Node 0x07 (ALC285 ADC compatibility): Power D0, 48kHz/16-bit/2ch format, Unmute */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x07, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x07, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x07, HDA_VERB_SET_AMP_GAIN_MUTE, 0x7000 | (0 << 8) | 0x1F), &dummy);

    SofLogBreadcrumb("[HDA-Init] Codec routing & widgets initialized successfully!\n");
    return status;
}

NTSTATUS SofHdaStartConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ BOOLEAN IsCapture,
    _In_ ULONG StreamTag
)
{
    ULONG dummy = 0;
    ULONG streamChan = ((StreamTag & 0x0F) << 4) | 0; /* Channel 0 (stereo pair) */
    char msg[128];

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-Conv] StartConverter: CAD=%u, IsCapture=%u, StreamTag=%u (0x%02X)\n",
        CodecAddress, IsCapture, StreamTag, streamChan);
    SofLogBreadcrumb(msg);

    if (IsCapture) {
        /* Set Format: 48kHz, 16-bit, 2ch */
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x08, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);

        /* Assign StreamTag to ADC 1 (0x08) ONLY to prevent SDI bus collision */
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x08, HDA_VERB_SET_CONV_STREAM_CHAN, streamChan), &dummy);

        /* Ensure ADC 2 (0x09) and ALC285 ADC (0x07) are idle (StreamTag 0) */
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x09, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x07, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);

        /* Readback diagnostics */
        ULONG adcStream = 0, adcFmt = 0, muxConn = 0;
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x08, HDA_VERB_GET_CONV_STREAM_CHAN, 0), &adcStream);
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x08, HDA_VERB_GET_CONV_FMT, 0x0000), &adcFmt);
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x23, HDA_VERB_GET_CONN_SELECT, 0), &muxConn);
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[HDA-Conv] ADC 0x08 readback: StreamChan=0x%02X, Fmt=0x%04X, MUX 0x23 Sel=%u\n",
            adcStream, adcFmt, muxConn);
        SofLogBreadcrumb(msg);
    } else {
        /* Set Format: 48kHz, 16-bit, 2ch */
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x02, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x03, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);

        /* Assign StreamTag to DAC 1 (0x02 - Front) and DAC 2 (0x03 - Headphone) */
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x02, HDA_VERB_SET_CONV_STREAM_CHAN, streamChan), &dummy);
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x03, HDA_VERB_SET_CONV_STREAM_CHAN, streamChan), &dummy);

        /* Readback diagnostics */
        ULONG dacStream = 0, dacFmt = 0;
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x02, HDA_VERB_GET_CONV_STREAM_CHAN, 0), &dacStream);
        SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, 0x02, HDA_VERB_GET_CONV_FMT, 0x0000), &dacFmt);
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[HDA-Conv] DAC 0x02 readback: StreamChan=0x%02X, Fmt=0x%04X\n",
            dacStream, dacFmt);
        SofLogBreadcrumb(msg);
    }

    return STATUS_SUCCESS;
}

NTSTATUS SofHdaStopConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ BOOLEAN IsCapture
)
{
    ULONG dummy = 0;
    char msg[128];

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-Conv] StopConverter: CAD=%u, IsCapture=%u (StreamTag=0)\n",
        CodecAddress, IsCapture);
    SofLogBreadcrumb(msg);

    if (IsCapture) {
        /* Set StreamTag = 0 to idle ADCs */
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x08, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x09, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x07, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);
    } else {
        /* Set StreamTag = 0 to idle DACs */
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x02, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x03, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);
    }

    return STATUS_SUCCESS;
}

NTSTATUS SofHdaProbeDisplayCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _Out_ PULONG VendorId,
    _Out_ PULONG RevisionId
)
{
    NTSTATUS status;
    ULONG vid = 0;
    ULONG rev = 0;
    ULONG verb;

    if (!VendorId || !RevisionId) return STATUS_INVALID_PARAMETER;

    /* Verb: Get Parameter (0xF00), Param 0x00 = Vendor ID */
    verb = (CodecAddress << 28) | (0 << 20) | (HDA_VERB_GET_PARAM << 8) | HDA_PARAM_VENDOR_ID;
    status = SofHdaSendVerb(Ctx, verb, &vid);
    if (!NT_SUCCESS(status) || vid == 0 || vid == 0xFFFFFFFF) {
        char msg[128];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[HDA-DisplayProbe] Codec CAD=%u probe failed: status=0x%08x, vid=0x%08x\n",
            CodecAddress, status, vid);
        SofLogBreadcrumb(msg);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    /* Verb: Get Parameter (0xF00), Param 0x02 = Revision ID */
    verb = (CodecAddress << 28) | (0 << 20) | (HDA_VERB_GET_PARAM << 8) | HDA_PARAM_REV_ID;
    status = SofHdaSendVerb(Ctx, verb, &rev);
    if (!NT_SUCCESS(status)) {
        rev = 0;
    }

    *VendorId = vid;
    *RevisionId = rev;

    char pmsg[128];
    RtlStringCbPrintfA(pmsg, sizeof(pmsg),
        "[HDA-DisplayProbe] Intel Display Audio Codec CAD=%u: VendorID=0x%08X, RevID=0x%08X\n",
        CodecAddress, vid, rev);
    SofLogBreadcrumb(pmsg);

    return STATUS_SUCCESS;
}

NTSTATUS SofHdaGetDisplayPinSense(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG PinNid,
    _Out_ PBOOLEAN Connected,
    _Out_ PBOOLEAN EldValid
)
{
    NTSTATUS status;
    ULONG resp = 0;

    if (!Connected || !EldValid) return STATUS_INVALID_PARAMETER;
    *Connected = FALSE;
    *EldValid = FALSE;

    /* Verb: Get Pin Sense (0xF09) */
    status = SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, PinNid, HDA_VERB_GET_PIN_SENSE, 0x00), &resp);
    if (NT_SUCCESS(status)) {
        if (resp & HDA_PIN_SENSE_PRESENCE) {
            *Connected = TRUE;
        }
        if (resp & HDA_PIN_SENSE_ELD_VALID) {
            *EldValid = TRUE;
        }
    }
    return status;
}

NTSTATUS SofHdaGetDisplayEld(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG PinNid,
    _Out_writes_to_(MaxLen, *ActualLen) PUCHAR EldBuffer,
    _In_ ULONG MaxLen,
    _Out_ PULONG ActualLen
)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    ULONG eldLen = 0;

    if (!Ctx || !EldBuffer || !ActualLen || MaxLen == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    *ActualLen = 0;
    RtlZeroMemory(EldBuffer, MaxLen);

    /* Read byte-by-byte via verb 0xF2F */
    for (i = 0; i < MaxLen; i++) {
        ULONG byteVal = 0;
        status = SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, PinNid, HDA_VERB_GET_HDMI_ELD, i), &byteVal);
        if (!NT_SUCCESS(status)) {
            break;
        }
        EldBuffer[i] = (UCHAR)(byteVal & 0xFF);
        eldLen = i + 1;

        /* If byte 4 (Baseline ELD length in DWORDs), calculate expected total bytes */
        if (i == 4) {
            ULONG baselineDwords = EldBuffer[4] & 0x1F;
            ULONG totalBytes = 4 + (baselineDwords * 4);
            if (totalBytes > 4 && totalBytes < MaxLen) {
                MaxLen = totalBytes;
            }
        }
    }

    *ActualLen = eldLen;
    return (eldLen > 0) ? STATUS_SUCCESS : status;
}

VOID SofHdaParseMonitorName(
    _In_reads_(EldLen) const UCHAR *EldBuffer,
    _In_ ULONG EldLen,
    _Out_writes_(MaxChars) PWCHAR NameOut,
    _In_ ULONG MaxChars
)
{
    ULONG nameLen = 0;
    ULONG i;

    if (!EldBuffer || EldLen < 20 || !NameOut || MaxChars == 0) {
        return;
    }

    /* In ELD v2: Byte 4 bits 7:5 specifies MNL (Monitor Name Length) */
    nameLen = (EldBuffer[4] >> 5) & 0x07;
    if (nameLen == 0 && EldLen > 20) {
        nameLen = EldBuffer[4] & 0x1F;
    }
    if (nameLen > 16) {
        nameLen = 16;
    }
    if (nameLen >= MaxChars) {
        nameLen = MaxChars - 1;
    }

    /* Convert ASCII monitor name at byte offset 20 to UTF-16 */
    if (20 + nameLen <= EldLen && nameLen > 0) {
        for (i = 0; i < nameLen; i++) {
            UCHAR ch = EldBuffer[20 + i];
            if (ch >= 0x20 && ch <= 0x7E) {
                NameOut[i] = (WCHAR)ch;
            } else {
                break;
            }
        }
        NameOut[i] = L'\0';
    }
}

USHORT SofHdaComputeFormatWord(
    _In_ ULONG SampleRate,
    _In_ ULONG BitsPerSample,
    _In_ ULONG Channels
)
{
    USHORT fmt = 0;

    /* Base rate & multiplier: bit 14 = 44.1kHz base, bits 13:11 = multiplier, bits 10:8 = divisor */
    switch (SampleRate) {
    case 44100:
        fmt |= (1 << 14); /* 44.1 kHz base */
        break;
    case 88200:
        fmt |= (1 << 14) | (1 << 11); /* 44.1 kHz * 2 */
        break;
    case 176400:
        fmt |= (1 << 14) | (3 << 11); /* 44.1 kHz * 4 */
        break;
    case 96000:
        fmt |= (1 << 11); /* 48 kHz * 2 */
        break;
    case 192000:
        fmt |= (3 << 11); /* 48 kHz * 4 */
        break;
    case 32000:
        /* 48 kHz * 2 / 3 */
        fmt |= (1 << 11) | (2 << 8);
        break;
    case 48000:
    default:
        /* 48 kHz base */
        break;
    }

    /* Bits per sample: bits 6:4 */
    switch (BitsPerSample) {
    case 32:
        fmt |= (4 << 4); /* 32 bits per sample */
        break;
    case 24:
        fmt |= (3 << 4); /* 24 bits per sample */
        break;
    case 20:
        fmt |= (2 << 4); /* 20 bits per sample */
        break;
    case 16:
    default:
        fmt |= (1 << 4); /* 16 bits per sample */
        break;
    }

    /* Channels: bits 3:0 (0 = 1 ch, 1 = 2 ch, ..., 7 = 8 ch) */
    if (Channels >= 1 && Channels <= 8) {
        fmt |= (USHORT)((Channels - 1) & 0x0F);
    } else {
        fmt |= 0x01; /* 2 channels default */
    }

    return fmt;
}

VOID SofHdaParseEldAudioCapabilities(
    _In_reads_(EldLen) const UCHAR *EldBuffer,
    _In_ ULONG EldLen,
    _Out_ PULONG SampleRatesMask,
    _Out_ PULONG BitDepthsMask,
    _Out_ PULONG MaxChannels
)
{
    ULONG sadCount = 0;
    ULONG mnl = 0;
    ULONG offset = 0;
    ULONG parsedRates = 0;
    ULONG parsedBits = 0;
    ULONG maxChan = 2;

    if (!SampleRatesMask || !BitDepthsMask || !MaxChannels) {
        return;
    }

    /* Default fallbacks: 44.1k-192k, 16/24-bit, 2-ch */
    *SampleRatesMask = 0x7E;
    *BitDepthsMask = 0x05;
    *MaxChannels = 2;

    if (!EldBuffer || EldLen < 20) {
        return;
    }

    /* ELD Byte 4: bits 7:5 = SAD Count, bits 4:0 = MNL (Monitor Name Length) */
    sadCount = (EldBuffer[4] >> 5) & 0x07;
    mnl = EldBuffer[4] & 0x1F;
    if (mnl > 16) {
        mnl = 16;
    }

    offset = 20 + mnl;
    if (offset >= EldLen || sadCount == 0) {
        return;
    }

    for (ULONG i = 0; i < sadCount; i++) {
        if (offset + 3 > EldLen) {
            break;
        }

        UCHAR sad0 = EldBuffer[offset];
        UCHAR sad1 = EldBuffer[offset + 1];
        UCHAR sad2 = EldBuffer[offset + 2];
        UCHAR formatCode = (sad0 >> 3) & 0x0F;

        /* CEA-861 Format Code 1 = LPCM */
        if (formatCode == 1) {
            ULONG ch = (sad0 & 0x07) + 1;
            if (ch > maxChan) {
                maxChan = ch;
            }

            /* Sampling frequencies: bits 0-6 = 32, 44.1, 48, 88.2, 96, 176.4, 192 kHz */
            parsedRates |= (sad1 & 0x7F);

            /* Bit depths: bits 0-2 = 16-bit, 20-bit, 24-bit */
            parsedBits |= (sad2 & 0x07);
        }

        offset += 3;
    }

    if (parsedRates != 0) {
        *SampleRatesMask = parsedRates;
    }
    if (parsedBits != 0) {
        *BitDepthsMask = parsedBits;
    }
    *MaxChannels = maxChan;
}

NTSTATUS SofHdaInitDisplayCodec(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress
)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG dummy = 0;
    ULONG rootSub = 0;
    ULONG afgNid = 0x01;
    ULONG afgSub = 0;
    ULONG totalWidgets = 0;
    ULONG startWidget = 0;
    char msg[128];

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-DisplayInit] Initializing Intel Display Audio Codec CAD=%u (VendorID=0x%08X)...\n",
        CodecAddress, Ctx->GfxCodecVendorId);
    SofLogBreadcrumb(msg);

    /* Initialize display defaults with extended formats */
    for (ULONG i = 0; i < 3; i++) {
        Ctx->DisplayConnected[i] = FALSE;
        Ctx->DisplaySampleRates[i] = 0x7E; /* 44.1k, 48k, 88.2k, 96k, 176.4k, 192k */
        Ctx->DisplayBitDepths[i] = 0x05;   /* 16-bit, 24-bit */
        Ctx->DisplayMaxChannels[i] = 8;    /* Up to 8 channels / 7.1 */
        RtlStringCbPrintfW(Ctx->DisplayMonitorName[i], sizeof(Ctx->DisplayMonitorName[i]),
            L"Intel Display Audio %u", i + 1);
    }

    /* 1. Query Root (Node 0x00) Subnode Count to find Audio Function Group (AFG) */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, 0x00, HDA_VERB_GET_PARAM, HDA_PARAM_SUB_NODE_COUNT), &rootSub);
    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-DisplayInit] CAD=%u Root SubNodeParam=0x%08X (Count=%u, Start=0x%02X)\n",
        CodecAddress, rootSub, (rootSub >> 16) & 0xFF, rootSub & 0xFF);
    SofLogBreadcrumb(msg);

    if (((rootSub >> 16) & 0xFF) > 0) {
        afgNid = rootSub & 0xFF;
    }
    Ctx->DisplayAfgNid = afgNid;

    /* 2. AFG Node: Power State D0 */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, afgNid, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);

    /* 3. Query AFG Subnodes to discover all audio widgets */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, afgNid, HDA_VERB_GET_PARAM, HDA_PARAM_SUB_NODE_COUNT), &afgSub);
    totalWidgets = (afgSub >> 16) & 0xFF;
    startWidget = afgSub & 0xFF;
    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-DisplayInit] AFG 0x%02X SubNodeParam=0x%08X (Widgets=%u, Start=0x%02X)\n",
        afgNid, afgSub, totalWidgets, startWidget);
    SofLogBreadcrumb(msg);

    Ctx->DisplayCvtCount = 0;
    Ctx->DisplayPinCount = 0;
    Ctx->ActiveDisplayPin = 0;

    for (ULONG nid = startWidget; nid < startWidget + totalWidgets; nid++) {
        ULONG widgetCap = 0;
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, nid, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WIDGET_CAP), &widgetCap);
        ULONG wType = (widgetCap >> 20) & 0x0F;

        /* Power widget to D0 */
        SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, nid, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);

        if (wType == 0) {
            /* Audio Output (DAC Converter) */
            if (Ctx->DisplayCvtCount < 4) {
                Ctx->DisplayCvtNid[Ctx->DisplayCvtCount++] = nid;
            }
            /* Program default format and idle stream tag */
            SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, nid, HDA_VERB_SET_CONV_FMT, 0x0011), &dummy);
            SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, nid, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);

            RtlStringCbPrintfA(msg, sizeof(msg),
                "[HDA-DisplayTree] Cvt 0x%02X: Cap=0x%08X (Audio Output)\n", nid, widgetCap);
            SofLogBreadcrumb(msg);
        } else if (wType == 4) {
            /* Pin Complex */
            BOOLEAN connected = FALSE;
            BOOLEAN eldValid = FALSE;
            ULONG pinIdx = Ctx->DisplayPinCount;

            if (Ctx->DisplayPinCount < 4) {
                Ctx->DisplayPinNid[Ctx->DisplayPinCount++] = nid;
            }

            /* Enable pin output (0x40) */
            SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, nid, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x40), &dummy);

            /* Query Pin Sense */
            SofHdaGetDisplayPinSense(Ctx, CodecAddress, nid, &connected, &eldValid);

            ULONG connList = 0;
            SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, nid, HDA_VERB_GET_CONN_LIST, 0), &connList);

            RtlStringCbPrintfA(msg, sizeof(msg),
                "[HDA-DisplayTree] Pin 0x%02X: Cap=0x%08X, Connected=%s, EldValid=%s, ConnList=0x%08X\n",
                nid, widgetCap, connected ? "TRUE" : "FALSE", eldValid ? "TRUE" : "FALSE", connList);
            SofLogBreadcrumb(msg);

            /* Check and read ELD if available */
            if (pinIdx < 3) {
                Ctx->DisplayConnected[pinIdx] = connected;
                if (connected || eldValid) {
                    UCHAR eldBuf[128];
                    ULONG actualEld = 0;
                    if (NT_SUCCESS(SofHdaGetDisplayEld(Ctx, CodecAddress, nid, eldBuf, sizeof(eldBuf), &actualEld)) && actualEld >= 20) {
                        SofHdaParseMonitorName(eldBuf, actualEld, Ctx->DisplayMonitorName[pinIdx], 32);
                        SofHdaParseEldAudioCapabilities(eldBuf, actualEld,
                            &Ctx->DisplaySampleRates[pinIdx],
                            &Ctx->DisplayBitDepths[pinIdx],
                            &Ctx->DisplayMaxChannels[pinIdx]);
                        RtlStringCbPrintfA(msg, sizeof(msg),
                            "[HDA-DisplayEld] Pin 0x%02X: Read %u bytes ELD, Monitor='%ws', Rates=0x%02X, Bits=0x%02X, MaxCh=%u\n",
                            nid, actualEld, Ctx->DisplayMonitorName[pinIdx],
                            Ctx->DisplaySampleRates[pinIdx], Ctx->DisplayBitDepths[pinIdx], Ctx->DisplayMaxChannels[pinIdx]);
                        SofLogBreadcrumb(msg);
                    }
                }
            }

            if (connected && Ctx->ActiveDisplayPin == 0) {
                Ctx->ActiveDisplayPin = nid;
            }
        }
    }

    if (Ctx->ActiveDisplayPin == 0) {
        if (Ctx->DisplayPinCount > 0) {
            Ctx->ActiveDisplayPin = Ctx->DisplayPinNid[0];
            Ctx->DisplayConnected[0] = TRUE;
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[HDA-DisplayInit] Primary display active on Pin 0x%02X (HDMI 1)\n", Ctx->ActiveDisplayPin);
        } else {
            Ctx->ActiveDisplayPin = 0x04;
            Ctx->DisplayConnected[0] = TRUE;
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[HDA-DisplayInit] Fallback ActiveDisplayPin=0x04 (HDMI 1)\n");
        }
        SofLogBreadcrumb(msg);
    } else {
        Ctx->DisplayConnected[0] = TRUE;
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[HDA-DisplayInit] Active connected display detected on Pin 0x%02X!\n", Ctx->ActiveDisplayPin);
        SofLogBreadcrumb(msg);
    }

    /* Connect first converter to ActiveDisplayPin */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, Ctx->ActiveDisplayPin, HDA_VERB_SET_CONN_SELECT, 0x00), &dummy);

    return status;
}

NTSTATUS SofHdaStartDisplayConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG LinkIndex,
    _In_ ULONG StreamTag,
    _In_opt_ const struct _SOF_AUDIO_FORMAT_PARAMS *Format
)
{
    ULONG dummy = 0;
    ULONG cvtNid;
    ULONG pinNid;
    ULONG streamChan = ((StreamTag & 0x0F) << 4) | 0; /* Channel 0 */
    USHORT convFmt = 0x0011;
    char msg[128];

    /* Map LinkIndex to converter */
    if (LinkIndex > 0 && LinkIndex <= Ctx->DisplayCvtCount) {
        cvtNid = Ctx->DisplayCvtNid[LinkIndex - 1];
    } else if (Ctx->DisplayCvtCount > 0) {
        cvtNid = Ctx->DisplayCvtNid[0];
    } else {
        cvtNid = 0x01 + LinkIndex;
    }

    /* Map to Pin */
    if (LinkIndex == 1 && Ctx->ActiveDisplayPin != 0) {
        pinNid = Ctx->ActiveDisplayPin;
    } else if (LinkIndex > 0 && LinkIndex <= Ctx->DisplayPinCount) {
        pinNid = Ctx->DisplayPinNid[LinkIndex - 1];
    } else {
        pinNid = 0x04 + LinkIndex;
    }

    /* Compute HDA Converter Format Word */
    if (Format != NULL) {
        convFmt = SofHdaComputeFormatWord(
            Format->SamplingFrequency,
            Format->ValidBitDepth,
            Format->ChannelsCount
        );
    }

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-DisplayConv] Start: Link=%u, Cvt=0x%02X, Pin=0x%02X, Tag=%u, Fmt=0x%04X (%uHz, %ubit, %uch)\n",
        LinkIndex, cvtNid, pinNid, StreamTag, convFmt,
        Format ? Format->SamplingFrequency : 48000,
        Format ? Format->ValidBitDepth : 16,
        Format ? Format->ChannelsCount : 2);
    SofLogBreadcrumb(msg);

    /* 1. Ensure Pin is Out Enabled */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, pinNid, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x40), &dummy);

    /* 2. Set Pin Connection Select to route cvtNid (idx 0 for first converter, etc.) */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, pinNid, HDA_VERB_SET_CONN_SELECT, (LinkIndex > 0 ? LinkIndex - 1 : 0)), &dummy);

    /* 3. Program Format */
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, cvtNid, HDA_VERB_SET_CONV_FMT, convFmt), &dummy);

    /* 4. Assign StreamTag & Channel */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, cvtNid, HDA_VERB_SET_CONV_STREAM_CHAN, streamChan), &dummy);

    /* 5. Readback diagnostics */
    ULONG readStream = 0, readFmt = 0, readPin = 0;
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, cvtNid, HDA_VERB_GET_CONV_STREAM_CHAN, 0), &readStream);
    SofHdaSendVerb(Ctx, HDA_VERB_4BIT(CodecAddress, cvtNid, HDA_VERB_GET_CONV_FMT, 0), &readFmt);
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, pinNid, HDA_VERB_GET_CONN_SELECT, 0), &readPin);
    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-DisplayConv] Readback: Cvt 0x%02X StreamChan=0x%02X, Fmt=0x%04X, Pin 0x%02X ConnSel=%u\n",
        cvtNid, readStream, readFmt, pinNid, readPin);
    SofLogBreadcrumb(msg);

    return STATUS_SUCCESS;
}

NTSTATUS SofHdaStopDisplayConverter(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG CodecAddress,
    _In_ ULONG LinkIndex
)
{
    ULONG dummy = 0;
    ULONG cvtNid;

    if (LinkIndex > 0 && LinkIndex <= Ctx->DisplayCvtCount) {
        cvtNid = Ctx->DisplayCvtNid[LinkIndex - 1];
    } else if (Ctx->DisplayCvtCount > 0) {
        cvtNid = Ctx->DisplayCvtNid[0];
    } else {
        cvtNid = 0x01 + LinkIndex;
    }

    char msg[128];
    RtlStringCbPrintfA(msg, sizeof(msg),
        "[HDA-DisplayConv] Stop: Link=%u, Cvt=0x%02X (StreamTag=0)\n",
        LinkIndex, cvtNid);
    SofLogBreadcrumb(msg);

    /* Set StreamTag = 0 to idle display converter */
    SofHdaSendVerb(Ctx, HDA_VERB(CodecAddress, cvtNid, HDA_VERB_SET_CONV_STREAM_CHAN, 0x00), &dummy);

    return STATUS_SUCCESS;
}

VOID SofCheckAndNotifyJackPresence(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx || !Ctx->PortEvents) {
        return;
    }

    /* 1. Check if registry override SimulateJackState is configured */
    ULONG simMask = 0xFFFFFFFF;
    WDFKEY key = NULL;
    NTSTATUS status = WdfDriverOpenParametersRegistryKey(
        WdfGetDriver(),
        KEY_READ,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key
    );
    if (NT_SUCCESS(status) && key != NULL) {
        UNICODE_STRING simValName;
        RtlInitUnicodeString(&simValName, L"SimulateJackState");
        WdfRegistryQueryULong(key, &simValName, &simMask);
        WdfRegistryClose(key);
    } else {
        /* Fallback: try device driver key */
        status = WdfDeviceOpenRegistryKey(
            Ctx->WdfDevice,
            PLUGPLAY_REGKEY_DRIVER,
            KEY_READ,
            WDF_NO_OBJECT_ATTRIBUTES,
            &key
        );
        if (NT_SUCCESS(status) && key != NULL) {
            UNICODE_STRING simValName;
            RtlInitUnicodeString(&simValName, L"SimulateJackState");
            WdfRegistryQueryULong(key, &simValName, &simMask);
            WdfRegistryClose(key);
        }
    }

    /* HDMI Pins: Pin 5 = HDMI 1, Pin 7 = HDMI 2, Pin 9 = HDMI 3 */
    for (ULONG i = 0; i < 3; i++) {
        ULONG bridgePinId = 5 + (i * 2);
        BOOLEAN newConnected = FALSE;

        if (simMask != 0xFFFFFFFF) {
            /* Simulated mode: Bit (2 + i) controls HDMI (i + 1) */
            newConnected = (simMask & (1 << (2 + i))) ? TRUE : FALSE;
        } else {
            /* Hardware sense mode: query GFX Codec CAD 2 only if in D0 */
            if (Ctx->CurrentPowerState == PowerDeviceD0 && Ctx->IDispCodecPresent && i < Ctx->DisplayPinCount) {
                ULONG pinNid = Ctx->DisplayPinNid[i];
                BOOLEAN eldValid = FALSE;
                SofHdaGetDisplayPinSense(Ctx, 2, pinNid, &newConnected, &eldValid);
            } else {
                newConnected = Ctx->DisplayConnected[i]; /* Retain state if unpowered */
            }
        }

        /* Check for state transition */
        if (newConnected != Ctx->DisplayConnected[i]) {
            char msg[128];
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[Jack] HDMI %u (Pin %u) connection state changed: %s -> %s\n",
                i + 1, bridgePinId,
                Ctx->DisplayConnected[i] ? "CONNECTED" : "UNPLUGGED",
                newConnected ? "CONNECTED" : "UNPLUGGED");
            SofLogBreadcrumb(msg);

            Ctx->DisplayConnected[i] = newConnected;

            /* If newly connected in physical hardware mode, re-read ELD if valid */
            if (newConnected && (simMask == 0xFFFFFFFF)) {
                if (i < Ctx->DisplayPinCount && Ctx->IDispCodecPresent) {
                    BOOLEAN pinConnected = FALSE, pinEldValid = FALSE;
                    SofHdaGetDisplayPinSense(Ctx, 2, Ctx->DisplayPinNid[i], &pinConnected, &pinEldValid);
                    if (pinConnected && pinEldValid) {
                        UCHAR eldBuf[128];
                        ULONG actualEld = 0;
                        if (NT_SUCCESS(SofHdaGetDisplayEld(Ctx, 2, Ctx->DisplayPinNid[i], eldBuf, sizeof(eldBuf), &actualEld)) && actualEld >= 20) {
                            SofHdaParseMonitorName(eldBuf, actualEld, Ctx->DisplayMonitorName[i], 32);
                            SofHdaParseEldAudioCapabilities(eldBuf, actualEld,
                                &Ctx->DisplaySampleRates[i],
                                &Ctx->DisplayBitDepths[i],
                                &Ctx->DisplayMaxChannels[i]);
                        }
                    }
                }
            }

            /* Signal PortCls / AudioEndpointBuilder */
            SofPortClsNotifyJackPresence(Ctx, bridgePinId);
        }
    }
}

#endif /* _KERNEL_MODE */
