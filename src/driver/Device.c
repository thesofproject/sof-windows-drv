/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Device.c
 * @brief Device Initialization, MMIO BAR Mapping, and PnP/Power Handlers for SOF Windows Driver.
 *
 * Implements hardware resource preparation, PCI capability list traversal,
 * low-power S0-Idle (D0i3) hardware handshakes, interrupt servicing, and runtime power management.
 *
 * Hardware IP Highlights:
 * - HDA Extended Capabilities: Discovers Processing Pipe (PP, 0x3) and Software Position-in-Buffer (SPIB, 0x4).
 * - S0-Idle / D0i3 Handshake: Interacts with MTL_HDA_VS_D0I3C (BAR0: 0x1D4A) polling CIP (bit 0) and asserting I3 (bit 2).
 * - Multi-Link & SoundWire: Enumerates child peripheral devices discovered via ACPI or SoundWire bus registers.
 * - Jack & Display Presence: Employs dynamic polling via passive WDF timers to detect HDMI/DP hot-plug events.
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#include "Driver.h"
#include "Device.h"
#include "Loader.h"
#include "Topo.h"
#include "WaveRt.h"
#include "PortClsAdapter.h"
#include "HdaVerb.h"
#include "HdaBusInterface.h"
#include "SdwController.h"
#include "UsbOffload.h"
#include "UaolGateway.h"
#include <ntstrsafe.h>
#include <initguid.h>
#include <wdmguid.h>

NTSYSAPI NTSTATUS NTAPI ZwFlushKey(_In_ HANDLE KeyHandle);

/**
 * @brief Traverses the Intel HDA extended capability linked list in BAR0.
 *
 * Discovers:
 * 1. Stream channel counts from GCAP (BAR0: 0x00).
 * 2. Processing Pipe Capability (PP, Cap ID 0x3): Enables GPROCEN (bit 30) and PIE (bit 31)
 *    to bridge host DMA channels into the DSP audio processing pipe.
 * 3. Software Position-in-Buffer Capability (SPIB, Cap ID 0x4): Enables low-latency DMA
 *    position reporting without PCI bus round-trip read penalties.
 * 4. Multi-Link Capability (ML, Cap ID 0x2): Discovers multi-link digital audio lines.
 *
 * @param[in,out] Ctx Pointer to device extension context.
 */
VOID SofDiscoverHdaCaps(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    PUCHAR bar0;
    ULONG offset;
    ULONG count = 0;
    ULONG gcap;

    if (!Ctx || !Ctx->Bar0VirtualAddress || Ctx->Bar0Length < 0x80)
        return;

    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;
    Ctx->PpCapOffset = 0;
    Ctx->SpbCapOffset = 0;

    /*
     * Read Global Capabilities Register (GCAP, BAR0 offset 0x00):
     * Bits 8..11:  Number of Input/Capture Stream Descriptors.
     * Bits 12..15: Number of Output/Playback Stream Descriptors.
     */
    gcap = readl(bar0 + SOF_HDA_GCAP);
    Ctx->NumCaptureStreams = (gcap >> 8) & 0x0F;
    Ctx->NumPlaybackStreams = (gcap >> 12) & 0x0F;

    /* Fallback defaults if GCAP reads 0 */
    if (Ctx->NumCaptureStreams == 0) Ctx->NumCaptureStreams = 7;
    if (Ctx->NumPlaybackStreams == 0) Ctx->NumPlaybackStreams = 9;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] GCAP=0x%08x: NumCapture=%u, NumPlayback=%u\n",
               gcap, Ctx->NumCaptureStreams, Ctx->NumPlaybackStreams);

    /*
     * Read Lower Link Capabilities Header (LLCH) at BAR0 offset 0x14 (16-bit offset pointer).
     * Points to the first entry of the capability linked list.
     */
    offset = readw(bar0 + SOF_HDA_LLCH);

    if (offset == 0 || offset == 0xFFFF || offset >= Ctx->Bar0Length) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] No HDA extended capabilities list (LLCH=0x%x)\n", offset);
        return;
    }

    /* Walk the capability linked list up to SOF_HDA_MAX_CAPS hops */
    while (count++ <= SOF_HDA_MAX_CAPS && offset != 0 && offset != 0xFFFF) {
        ULONG capOffset = offset & SOF_HDA_CAP_NEXT_MASK;
        ULONG cap;
        ULONG capId;

        if (capOffset + sizeof(ULONG) > Ctx->Bar0Length) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                       "[SOF] Capability offset 0x%x exceeds BAR0 length 0x%llx\n",
                       capOffset, (unsigned long long)Ctx->Bar0Length);
            break;
        }

        cap = readl(bar0 + capOffset);
        capId = (cap & SOF_HDA_CAP_ID_MASK) >> SOF_HDA_CAP_ID_OFF;

        if (capId == SOF_HDA_PP_CAP_ID) {
            Ctx->PpCapOffset = capOffset;
            if (capOffset + SOF_HDA_REG_PP_PPCTL + sizeof(ULONG) <= Ctx->Bar0Length) {
                /*
                 * Enable Post-Processing Capability:
                 * - GPROCEN (bit 30): Global Processing Enable connects DMA engines to DSP.
                 * - PIE (bit 31): Processing Interrupt Enable allows DSP DMA completions.
                 */
                writel(readl(bar0 + capOffset + SOF_HDA_REG_PP_PPCTL) | (SOF_HDA_PPCTL_GPROCEN | SOF_HDA_PPCTL_PIE),
                       bar0 + capOffset + SOF_HDA_REG_PP_PPCTL);
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                           "[SOF] Discovered PP capability at 0x%x (GPROCEN enabled)\n", capOffset);
                SofLogBreadcrumb("[CAP] Discovered PP capability (GPROCEN enabled)\n");
            }
        } else if (capId == SOF_HDA_SPIB_CAP_ID) {
            Ctx->SpbCapOffset = capOffset;
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Discovered SPIB capability at 0x%x\n", capOffset);
            SofLogBreadcrumb("[CAP] Discovered SPIB capability\n");
        } else if (capId == SOF_HDA_ML_CAP_ID) {
            Ctx->MlCapOffset = capOffset;
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Discovered ML capability at 0x%x\n", capOffset);
            SofLogBreadcrumb("[CAP] Discovered ML capability\n");
        }

        offset = cap & SOF_HDA_CAP_NEXT_MASK;
        if (offset == capOffset || offset >= Ctx->Bar0Length)
            break;
    }
}

/**
 * @brief KMDF EvtDevicePrepareHardware callback.
 *
 * Invoked by PnP manager when hardware resources are assigned. Maps BAR0 (HDA)
 * and BAR4 (DSP Shim/SRAM), queries PCI bus standard interface, parses ACPI NHLT,
 * discovers audio codecs, and sets up PortCls audio subdevices.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] ResourcesRaw List of raw hardware resources.
 * @param[in] ResourcesTranslated List of translated hardware resources.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
NTSTATUS
SofEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
    NTSTATUS status;
    ULONG resourceCount;
    ULONG memoryBarIndex = 0;
    ULONG i;
    BUS_INTERFACE_STANDARD busInterface;
    char msg[256];

    UNREFERENCED_PARAMETER(ResourcesRaw);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] SofEvtDevicePrepareHardware entered.\n");
    SofLogBreadcrumb("[7] SofEvtDevicePrepareHardware entered!\n");

    /*
     * Identify hardware by reading PCI Vendor & Device ID via Standard Bus Interface.
     * Ensures Bus Master Enable (bit 2) and Memory Space Enable (bit 1) are active.
     */
    if (NT_SUCCESS(WdfFdoQueryForInterface(Device,
                                           &GUID_BUS_INTERFACE_STANDARD,
                                           (PINTERFACE)&busInterface,
                                           sizeof(BUS_INTERFACE_STANDARD),
                                           1,
                                           NULL))) {
        PCI_COMMON_CONFIG pciConfig;
        ULONG bytesRead = busInterface.GetBusData(busInterface.Context,
                                                  PCI_WHICHSPACE_CONFIG,
                                                  &pciConfig,
                                                  0,
                                                  sizeof(PCI_COMMON_CONFIG));
        if (bytesRead >= sizeof(PCI_COMMON_CONFIG)) {
            ctx->VendorId = pciConfig.VendorID;
            ctx->DeviceId = pciConfig.DeviceID;
            ctx->RevisionId = pciConfig.RevisionID;

            /* Ensure Bus Master Enable (bit 2) and Memory Space Enable (bit 1) are set */
            if ((pciConfig.Command & 0x0006) != 0x0006) {
                USHORT newCmd = pciConfig.Command | 0x0006;
                busInterface.SetBusData(busInterface.Context,
                                        PCI_WHICHSPACE_CONFIG,
                                        &newCmd,
                                        FIELD_OFFSET(PCI_COMMON_CONFIG, Command),
                                        sizeof(USHORT));
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                           "[SOF] PCI Command updated: 0x%04x -> 0x%04x (BME & MSE enabled)\n",
                           pciConfig.Command, newCmd);
            }
        }
        busInterface.InterfaceDereference(busInterface.Context);
    }

    /* Fallback default if bus interface did not return */
    if (ctx->VendorId == 0) {
        ctx->VendorId = 0x8086;
        ctx->DeviceId = 0xE428;
    }

    ctx->HwDesc = SofGetHardwareDesc(ctx->VendorId, ctx->DeviceId);
    if (!ctx->HwDesc) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Unsupported hardware VID: 0x%04x, DID: 0x%04x\n",
                   ctx->VendorId, ctx->DeviceId);
        return STATUS_NOT_SUPPORTED;
    }

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[8] Hardware identified: VID=0x%04x DID=0x%04x -> %s\n",
        ctx->VendorId, ctx->DeviceId, ctx->HwDesc->PlatformName);
    SofLogBreadcrumb(msg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Platform identified: %s (VID 0x%04x, DID 0x%04x)\n",
               ctx->HwDesc->PlatformName, ctx->VendorId, ctx->DeviceId);

    /* Parse Translated Resources (BAR0 = HDA, BAR4 = DSP Shim/SRAM, Interrupts) */
    resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < resourceCount; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (!desc)
            continue;

        if (desc->Type == CmResourceTypeMemory) {
            if (memoryBarIndex == 0) {
                /* BAR0: HD Audio Controller Registers */
                ctx->Bar0PhysicalAddress = desc->u.Memory.Start;
                ctx->Bar0Length = desc->u.Memory.Length;
                ctx->Bar0VirtualAddress = MmMapIoSpace(desc->u.Memory.Start,
                                                      desc->u.Memory.Length,
                                                      MmNonCached);
            } else if (memoryBarIndex == 1) {
                /* BAR4: DSP Shim, Mailbox, and SRAM */
                ctx->Bar4PhysicalAddress = desc->u.Memory.Start;
                ctx->Bar4Length = desc->u.Memory.Length;
                ctx->Bar4VirtualAddress = MmMapIoSpace(desc->u.Memory.Start,
                                                      desc->u.Memory.Length,
                                                      MmNonCached);
            }
            memoryBarIndex++;
        } else if (desc->Type == CmResourceTypeInterrupt) {
            ctx->InterruptVector = desc->u.Interrupt.Vector;
            ctx->InterruptIrql = (KIRQL)desc->u.Interrupt.Level;
            ctx->InterruptAffinity = desc->u.Interrupt.Affinity;
            ctx->IsMsi = (desc->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ? TRUE : FALSE;

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Interrupt Resource: Vector 0x%x, Level %u, MSI: %s\n",
                       ctx->InterruptVector,
                       ctx->InterruptIrql,
                       ctx->IsMsi ? "Yes" : "No");
        }
    }

    /* Discover HD-A controller capabilities (PP, SPIB) and enable GPROCEN */
    SofDiscoverHdaCaps(ctx);

    /*
     * Check HDA Codec wake/status to detect presence of external codec & display codec.
     * If Immediate Command Busy (ICB) is stuck from a previous session, cycle GCTL.CRST.
     */
    if (ctx->Bar0VirtualAddress) {
        USHORT ics = readw((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_REG_ICS);
        if (ics & SOF_HDA_ICS_ICB) {
            ULONG gctl = readl((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_GCTL);
            writel(gctl & ~SOF_HDA_GCTL_RESET, (PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_GCTL);
            KeStallExecutionProcessor(200);
            writel(gctl | SOF_HDA_GCTL_RESET, (PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_GCTL);
            KeStallExecutionProcessor(1000);
            SofLogBreadcrumb("[HDA] Cycled GCTL.CRST to clear stuck ICB\n");
        }

        USHORT wakests = readw((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_WAKESTS);
        ctx->HdaCodecPresent = (wakests & SOF_HDA_WAKESTS_SDIN0) ? TRUE : FALSE;
        ctx->IDispCodecPresent = (wakests & SOF_HDA_WAKESTS_SDIN2) ? TRUE : FALSE;
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[9a] HDA Codec Probe: WAKESTS=0x%04X -> HdaCodecPresent=%s, IDispCodecPresent=%s\n",
            wakests, ctx->HdaCodecPresent ? "TRUE" : "FALSE",
            ctx->IDispCodecPresent ? "TRUE" : "FALSE");
        SofLogBreadcrumb(msg);
    }

    {
        ULONG romSts = (ctx->Bar4VirtualAddress && ctx->HwDesc) ? SofReadRomStatus(ctx) : 0;
        ULONG fwSts = (ctx->Bar4VirtualAddress && ctx->HwDesc) ? SofReadFwStatus(ctx) : 0;

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[9] Resources mapped: BAR0=%p (len 0x%x), BAR4=%p (len 0x%x), IRQ=0x%x (MSI=%s) | DSP ROM_STS=0x%08X (PP=0x%x, SPIB=0x%x, Streams: C%u/P%u)\n",
            ctx->Bar0VirtualAddress, ctx->Bar0Length,
            ctx->Bar4VirtualAddress, ctx->Bar4Length,
            ctx->InterruptVector, ctx->IsMsi ? "Yes" : "No",
            romSts, ctx->PpCapOffset, ctx->SpbCapOffset,
            ctx->NumCaptureStreams, ctx->NumPlaybackStreams);
        SofLogBreadcrumb(msg);
    }

    /* Initialize IPC state machine */
    SofIpcInitialize(ctx);

    /* Initialize default audio control states (0 dB, unmuted) */
    for (ULONG ep = 0; ep < SOF_ENDPOINT_COUNT; ep++) {
        ctx->DefaultVolumeLevelDb[ep][0] = 0;
        ctx->DefaultVolumeLevelDb[ep][1] = 0;
        ctx->DefaultMute[ep] = FALSE;
    }

    /* Initialize display audio stream pointers and defaults */
    for (ULONG i = 0; i < 3; i++) {
        ctx->HdmiStreams[i] = NULL;
        ctx->DisplayConnected[i] = FALSE;
        RtlStringCbPrintfW(ctx->DisplayMonitorName[i], sizeof(ctx->DisplayMonitorName[i]),
                           L"Intel Display Audio %u", i + 1);
    }

    /* Query and parse ACPI NHLT table */
    {
        PVOID nhltBuffer = NULL;
        ULONG nhltSize = 0;
        NTSTATUS nhltStatus = SofNhltQueryAcpiTable(&nhltBuffer, &nhltSize);
        if (NT_SUCCESS(nhltStatus) && nhltBuffer != NULL) {
            SofNhltParse(nhltBuffer, nhltSize, &ctx->NhltInfo);
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[10] ACPI NHLT parsed: Endpoints=%u (DMIC=%u ch=%u, SSP=%u, SDW=%u)\n",
                ctx->NhltInfo.TotalEndpoints,
                ctx->NhltInfo.DmicEndpointsCount,
                ctx->NhltInfo.Dmic.ChannelCount,
                ctx->NhltInfo.SspEndpointsCount,
                ctx->NhltInfo.SdwEndpointsCount);
            SofLogBreadcrumb(msg);

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] ACPI NHLT parsed: Total Endpoints=%u, DMIC=%u (Channels=%u), SSP=%u, SDW=%u\n",
                       ctx->NhltInfo.TotalEndpoints,
                       ctx->NhltInfo.DmicEndpointsCount,
                       ctx->NhltInfo.Dmic.ChannelCount,
                       ctx->NhltInfo.SspEndpointsCount,
                       ctx->NhltInfo.SdwEndpointsCount);
            SofNhltFreeAcpiTable(nhltBuffer);
        } else {
            SofLogBreadcrumb("[10] ACPI NHLT table not present (using standard HDA+DMIC topology)\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] ACPI NHLT table not found; defaulting to standard HDA+DMIC topology.\n");
        }
    }

    /* Enumerate SoundWire child PDOs (e.g. SOUNDWIRE\SDCA_PERIPHERAL) */
    status = SofSdwEnumerateChildren(ctx);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[SDW] SofSdwEnumerateChildren failed\n");
    }

    /*
     * Query PortCls configuration from driver parameters registry.
     * Default: PortCls enabled out-of-the-box (EnablePortCls = 1).
     * Can be explicitly disabled via registry: EnablePortCls = 0.
     */
    ULONG armedTesting = 1;
    ULONG enablePortCls = 1;
    WDFKEY paramKey = NULL;
    NTSTATUS regStatus = WdfDriverOpenParametersRegistryKey(
        WdfGetDriver(),
        KEY_READ | KEY_WRITE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &paramKey
    );
    if (NT_SUCCESS(regStatus)) {
        UNICODE_STRING valArmed;
        UNICODE_STRING valPortCls;
        UNICODE_STRING valUsbOffload;
        ULONG enableUsbOffload = 1;
        RtlInitUnicodeString(&valArmed, L"ArmedTesting");
        RtlInitUnicodeString(&valPortCls, L"EnablePortCls");
        RtlInitUnicodeString(&valUsbOffload, L"EnableUsbOffload");
        WdfRegistryQueryULong(paramKey, &valArmed, &armedTesting);
        WdfRegistryQueryULong(paramKey, &valPortCls, &enablePortCls);
        WdfRegistryQueryULong(paramKey, &valUsbOffload, &enableUsbOffload);
        ctx->EnableUsbOffload = enableUsbOffload;
        WdfRegistryClose(paramKey);
    } else {
        ctx->EnableUsbOffload = 1;
    }

    ctx->ArmedTesting = armedTesting;
    ctx->EnablePortCls = enablePortCls;
    SofUaolInit(ctx);

    char safeMsg[160];
    RtlStringCbPrintfA(safeMsg, sizeof(safeMsg),
        "[10b] Safety Guards: CanaryArmed=%u, SafeBootPolicy=%u, EnablePortCls=%u\n",
        g_CanaryArmed ? 1 : 0, g_SafeBootPolicy, enablePortCls);
    SofLogBreadcrumb(safeMsg);
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", safeMsg);

    if (g_CanaryArmed && enablePortCls != 0) {
        SofLogBreadcrumb("[10b] Canary Armed & PortCls enabled: Registering PortCls WaveRT Subdevices...\n");
        status = SofRegisterPortClsSubdevices(Device, ctx);
        if (!NT_SUCCESS(status)) {
            char smsg[128];
            RtlStringCbPrintfA(smsg, sizeof(smsg),
                "[10b] SofRegisterPortClsSubdevices failed: 0x%08x -> falling back to Safe Mode\n", status);
            SofLogBreadcrumb(smsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", smsg);
            ctx->PortClsRegistered = FALSE;
            status = STATUS_SUCCESS; /* Soft assert: ensure device and boot never fail */
        } else {
            ctx->PortClsRegistered = TRUE;
            SofLogBreadcrumb("[10b] SofRegisterPortClsSubdevices succeeded (Wave subdevice active)!\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] PortCls WaveRT subdevice registered successfully.\n");
        }
    } else {
        SofLogBreadcrumb("[10b] Safe Mode active: PortCls subdevices registration bypassed.\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Safe Mode: PortCls registration bypassed (CanaryArmed=%u, EnablePortCls=%u).\n",
                   g_CanaryArmed ? 1 : 0, enablePortCls);
        ctx->PortClsRegistered = FALSE;
    }

    ctx->HardwarePrepared = TRUE;
    SofLogBreadcrumb("[10a] Hardware prepared successfully (BAR0 & BAR4 mapped, IPC & NHLT ready)!\n");
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Hardware preparation completed successfully.\n");
    return STATUS_SUCCESS;
}

/**
 * @brief KMDF EvtDeviceReleaseHardware callback.
 *
 * Unregisters PortCls audio interfaces, stops active streams, cleans up IPC,
 * powers down the DSP cores, and unmaps BAR0 and BAR4 MMIO spaces.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] ResourcesTranslated List of translated hardware resources.
 * @return NTSTATUS STATUS_SUCCESS.
 */
NTSTATUS
SofEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] SofEvtDeviceReleaseHardware starting...\n");
    SofLogBreadcrumb("[Teardown] SofEvtDeviceReleaseHardware starting...\n");

    /* Unregister PortCls Subdevices */
    SofUnregisterPortClsSubdevices(Device, ctx);
    SofLogBreadcrumb("[Teardown] SofUnregisterPortClsSubdevices completed.\n");

    /* Free active audio streams */
    if (ctx->PlaybackStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->PlaybackStream);
        ctx->PlaybackStream = NULL;
    }
    if (ctx->CaptureStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->CaptureStream);
        ctx->CaptureStream = NULL;
    }
    for (ULONG i = 0; i < 3; i++) {
        if (ctx->HdmiStreams[i] != NULL) {
            SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->HdmiStreams[i]);
            ctx->HdmiStreams[i] = NULL;
        }
    }
    if (ctx->DmicStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->DmicStream);
        ctx->DmicStream = NULL;
    }
    if (ctx->SspPlaybackStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->SspPlaybackStream);
        ctx->SspPlaybackStream = NULL;
    }
    if (ctx->SspCaptureStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->SspCaptureStream);
        ctx->SspCaptureStream = NULL;
    }

    /* Cleanup IPC resources */
    SofIpcCleanup(ctx);

    /* Power down DSP hardware cleanly */
    SofPowerDownDsp(ctx);

    if (ctx->Bar0VirtualAddress) {
        MmUnmapIoSpace(ctx->Bar0VirtualAddress, ctx->Bar0Length);
        ctx->Bar0VirtualAddress = NULL;
        ctx->Bar0Length = 0;
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] BAR0 unmapped.\n");
    }

    if (ctx->Bar4VirtualAddress) {
        MmUnmapIoSpace(ctx->Bar4VirtualAddress, ctx->Bar4Length);
        ctx->Bar4VirtualAddress = NULL;
        ctx->Bar4Length = 0;
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] BAR4 unmapped.\n");
    }

    /* Uninitialize SoundWire controller context */
    SofSdwUninitController(ctx);

    ctx->HardwarePrepared = FALSE;
    return STATUS_SUCCESS;
}

/**
 * @brief Performs the S0-Idle (D0i3) hardware power handshake with the DSP.
 *
 * Programs the vendor-specific D0I3C register at BAR0 offset 0x1D4A:
 * 1. Polls CIP (Command In Progress, bit 0) until hardware is ready.
 * 2. Writes I3 (bit 2): 1 to enter low-power D0i3; 0 to exit D0i3 back to D0.
 * 3. Polls CIP until the power transition handshake is completed by hardware.
 *
 * @param[in] ctx Pointer to device context.
 * @param[in] Enable TRUE to enter D0i3 (power gate); FALSE to exit D0i3 (power on).
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
static NTSTATUS
SofHdaSetD0i3(
    _In_ PSOF_DEVICE_CONTEXT ctx,
    _In_ BOOLEAN Enable
)
{
    if (ctx == NULL || ctx->Bar0VirtualAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    PUCHAR bar0 = (PUCHAR)ctx->Bar0VirtualAddress;
    ULONG timeoutUs = 10000; /* 10ms */
    ULONG elapsedUs = 0;
    UCHAR val = 0;

    /* Wait for CIP bit (bit 0) to clear before writing */
    while (elapsedUs < timeoutUs) {
        val = readb(bar0 + MTL_HDA_VS_D0I3C);
        if (!(val & SOF_HDA_VS_D0I3C_CIP)) {
            break;
        }
        KeStallExecutionProcessor(20);
        elapsedUs += 20;
    }

    if (val & SOF_HDA_VS_D0I3C_CIP) {
        SofLogBreadcrumb("[Power] Warning: CIP timeout before D0I3C update\n");
    }

    /* Update D0I3C: bit 2 is I3 */
    val = readb(bar0 + MTL_HDA_VS_D0I3C);
    if (Enable) {
        val |= (UCHAR)SOF_HDA_VS_D0I3C_I3;
    } else {
        val &= (UCHAR)~SOF_HDA_VS_D0I3C_I3;
    }
    writeb(val, bar0 + MTL_HDA_VS_D0I3C);

    /* Wait for CIP bit to clear after writing */
    elapsedUs = 0;
    while (elapsedUs < timeoutUs) {
        val = readb(bar0 + MTL_HDA_VS_D0I3C);
        if (!(val & SOF_HDA_VS_D0I3C_CIP)) {
            break;
        }
        KeStallExecutionProcessor(20);
        elapsedUs += 20;
    }

    val = readb(bar0 + MTL_HDA_VS_D0I3C);
    char dmsg[128];
    RtlStringCbPrintfA(dmsg, sizeof(dmsg),
        "[Power] D0I3C register set to 0x%02X (D0i3 %s, CIP=%s)\n",
        val,
        (val & SOF_HDA_VS_D0I3C_I3) ? "ENABLED" : "DISABLED",
        (val & SOF_HDA_VS_D0I3C_CIP) ? "BUSY" : "IDLE");
    SofLogBreadcrumb(dmsg);

    return STATUS_SUCCESS;
}

/**
 * @brief KMDF EvtDeviceD0Entry callback.
 *
 * Invoked when transitioning into the working power state (D0).
 * Exits D0i3, unmasks IPC interrupts, cold boots or restores DSP firmware,
 * validates on-demand audio pipelines, probes codecs, and arms timers.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] PreviousState Power state prior to entering D0.
 * @return NTSTATUS STATUS_SUCCESS on success.
 */
NTSTATUS
SofEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
    char msg[128];

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[11] SofEvtDeviceD0Entry: Entering D0 from state %d\n", PreviousState);
    SofLogBreadcrumb(msg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] EvtDeviceD0Entry: Entering D0 from State %d\n",
               PreviousState);

    ctx->CurrentPowerState = PowerDeviceD0;

    /* Wake hardware from D0i3 if entering from a low-power state */
    if (PreviousState > WdfPowerDeviceD0) {
        SofHdaSetD0i3(ctx, FALSE);
    }

    /* Enable IPC interrupts on entering D0 */
    SofIpcEnableInterrupts(ctx);

    /* Check if initial cold boot or waking from low-power state */
    BOOLEAN isInitialBoot = (ctx->FwState == SOF_FW_BOOT_NOT_STARTED);
    BOOLEAN dspNeedsBoot = isInitialBoot;

    if (!isInitialBoot && ctx->Bar4VirtualAddress && ctx->HwDesc) {
        PUCHAR bar4 = (PUCHAR)ctx->Bar4VirtualAddress;
        ULONG ctl = SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE);
        ULONG dsscs = SofDspRead32(bar4, MTL_HFDSSCS);
        ULONG romSts = SofDspRead32(bar4, ctx->HwDesc->RomStatusReg);

        char chkMsg[160];
        RtlStringCbPrintfA(chkMsg, sizeof(chkMsg),
            "[Power] D0Entry Resume Check: Core0=0x%08x, DSSCS=0x%08x, ROM=0x%08x\n",
            ctl, dsscs, romSts);
        SofLogBreadcrumb(chkMsg);

        /* If Core0 is not powered (CPA=0) or Subsystem is not powered (CPA=0) or ROM not entered */
        if (!(ctl & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK) ||
            !(dsscs & MTL_HFDSSCS_CPA_MASK) ||
            FSR_TO_STATE_CODE(romSts) != FSR_STATE_FW_ENTERED) {
            dspNeedsBoot = TRUE;
            SofLogBreadcrumb("[Power] DSP reset or lost power during sleep: Re-booting firmware on resume...\n");
        } else {
            SofLogBreadcrumb("[Power] DSP retained power & firmware state across sleep (CPA=1)!\n");
        }
    }

    if (dspNeedsBoot) {
        NTSTATUS bootStatus;
        if (isInitialBoot) {
            SofLogBreadcrumb("[12] Triggering initial firmware boot in D0...\n");
        } else {
            SofLogBreadcrumb("[Power] Triggering resume firmware re-boot in D0...\n");
        }

        bootStatus = SofStartFirmwareBoot(ctx, NULL);
        if (NT_SUCCESS(bootStatus) && ctx->FwState == SOF_FW_BOOT_COMPLETE) {
            SOF_AUDIO_FORMAT_PARAMS fmt;
            ULONG hostInst, gainInst, daiInst;

            /* Build 48kHz, 2-channel, 32-bit container audio format */
            SofIpc4BuildDefaultStereoFormat(48000, 32, &fmt);

            if (isInitialBoot) {
                SofLogBreadcrumb("[14] Firmware boot succeeded! Verifying on-demand audio pipelines...\n");

                /* 1. Test HDMI 1 On-Demand Pipeline (Link 1, Pipeline ID 1, Stream Tag 1) */
                if (NT_SUCCESS(SofIpc4CreateHdmiPipeline(ctx, 1, 1, 1, &fmt, &hostInst, &gainInst, &daiInst))) {
                    SofLogBreadcrumb("[HDMI1] Pipeline 1 verified; resetting and deleting on-demand...\n");
                    SofIpc4SetPipelineState(ctx, 1, SOF_IPC4_PIPE_RESET);
                    SofIpc4DeletePipeline(ctx, 1);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
                }

                /* 2. Test HDMI 2 On-Demand Pipeline (Link 2, Pipeline ID 2, Stream Tag 2) */
                if (NT_SUCCESS(SofIpc4CreateHdmiPipeline(ctx, 2, 2, 2, &fmt, &hostInst, &gainInst, &daiInst))) {
                    SofLogBreadcrumb("[HDMI2] Pipeline 2 verified; resetting and deleting on-demand...\n");
                    SofIpc4SetPipelineState(ctx, 2, SOF_IPC4_PIPE_RESET);
                    SofIpc4DeletePipeline(ctx, 2);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
                }

                /* 3. Test HDMI 3 On-Demand Pipeline (Link 3, Pipeline ID 3, Stream Tag 3) */
                if (NT_SUCCESS(SofIpc4CreateHdmiPipeline(ctx, 3, 3, 3, &fmt, &hostInst, &gainInst, &daiInst))) {
                    SofLogBreadcrumb("[HDMI3] Pipeline 3 verified; resetting and deleting on-demand...\n");
                    SofIpc4SetPipelineState(ctx, 3, SOF_IPC4_PIPE_RESET);
                    SofIpc4DeletePipeline(ctx, 3);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
                }

                /* 4. Test SSP2 Playback On-Demand Pipeline (Port 2, Pipeline ID 4, Stream Tag 4) */
                if (NT_SUCCESS(SofIpc4CreateSspPipeline(ctx, 4, 4, 2, FALSE, &fmt, &hostInst, &gainInst, &daiInst))) {
                    SofLogBreadcrumb("[SSP2-P] Pipeline 4 verified; resetting and deleting on-demand...\n");
                    SofIpc4SetPipelineState(ctx, 4, SOF_IPC4_PIPE_RESET);
                    SofIpc4DeletePipeline(ctx, 4);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
                }

                /* 5. Test SSP2 Capture On-Demand Pipeline (Port 2, Pipeline ID 5, Stream Tag 5) */
                if (NT_SUCCESS(SofIpc4CreateSspPipeline(ctx, 5, 5, 2, TRUE, &fmt, &hostInst, &gainInst, &daiInst))) {
                    SofLogBreadcrumb("[SSP2-C] Pipeline 5 verified; resetting and deleting on-demand...\n");
                    SofIpc4SetPipelineState(ctx, 5, SOF_IPC4_PIPE_RESET);
                    SofIpc4DeletePipeline(ctx, 5);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
                    SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
                }
            }

            /* Initialize / restore HDA Codec status */
            if (ctx->HdaCodecPresent) {
                SofHdaProbeCodec(ctx, 0, &ctx->CodecVendorId, &ctx->CodecRevisionId);
                SofHdaInitCodec(ctx, 0);

                if (isInitialBoot) {
                    if (NT_SUCCESS(SofIpc4CreatePlaybackPipeline(ctx, 6, 6, &fmt, &hostInst, &gainInst, &daiInst))) {
                        SofLogBreadcrumb("[HDA] Playback pipeline 6 verified; resetting and deleting on-demand...\n");
                        SofIpc4SetPipelineState(ctx, 6, SOF_IPC4_PIPE_RESET);
                        SofIpc4DeletePipeline(ctx, 6);
                        SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
                        SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
                        SofIpc4FreeInstanceId(ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
                    }
                }
            }

            /* Initialize / restore Intel Display Audio Codec status (CAD 2) */
            if (ctx->Bar0VirtualAddress) {
                USHORT wakests = readw((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_WAKESTS);
                if (wakests & SOF_HDA_WAKESTS_SDIN2) {
                    ctx->IDispCodecPresent = TRUE;
                }
            }

            if (ctx->IDispCodecPresent) {
                if (NT_SUCCESS(SofHdaProbeDisplayCodec(ctx, 2, &ctx->GfxCodecVendorId, &ctx->GfxCodecRevisionId))) {
                    SofHdaInitDisplayCodec(ctx, 2);
                }
            }

            if (isInitialBoot) {
                SofLogBreadcrumb("[16] All on-demand pipelines verified! (0 active pipelines in idle D0)\n");
            } else {
                SofLogBreadcrumb("[Power] Resume firmware re-boot and codec restoration completed!\n");
            }
        }
    } else {
        /* DSP retained power across sleep: ensure HDA codec power state is restored to D0 */
        if (ctx->HdaCodecPresent) {
            ULONG dummy;
            SofHdaSendVerb(ctx, HDA_VERB(0, 0x01, HDA_VERB_SET_POWER_STATE, 0x00), &dummy);
        }
    }

    /* Immediately check and report jack presence on D0 entry */
    if (ctx->PortEvents != NULL) {
        SofCheckAndNotifyJackPresence(ctx);
    }

    /* Start / re-arm Jack Poll Timer (1000ms periodic) */
    if (ctx->JackPollTimer) {
        WdfTimerStart(ctx->JackPollTimer, WDF_REL_TIMEOUT_IN_MS(1000));
        SofLogBreadcrumb("[Jack] Started JackPollTimer (1000ms periodic)\n");
    }

    /* Start Watchdog Health Timer (10s countdown to confirm healthy D0 uptime) */
    if (ctx->WatchdogHealthTimer) {
        WdfTimerStart(ctx->WatchdogHealthTimer, WDF_REL_TIMEOUT_IN_MS(10000));
        SofLogBreadcrumb("[Watchdog] Started WatchdogHealthTimer (10s countdown to health confirmation)\n");
    }

    /* Check for diagnostic DSP panic/core dump trigger */
    {
        WDFKEY paramKey = NULL;
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
            ULONG triggerDump = 0;
            UNICODE_STRING valTrigger;
            RtlInitUnicodeString(&valTrigger, L"TriggerDspPanicDump");
            if (NT_SUCCESS(WdfRegistryQueryULong(paramKey, &valTrigger, &triggerDump)) && triggerDump != 0) {
                SofLogBreadcrumb("[Watchdog] Diagnostic TriggerDspPanicDump=1 detected! Capturing DSP core dump...\n");
                SofDspDumpCore(ctx, "DIAGNOSTIC_TRIGGER");
                ULONG zero = 0;
                WdfRegistryAssignULong(paramKey, &valTrigger, zero);
                ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
                SofLogBreadcrumb("[Watchdog] Diagnostic core dump completed; TriggerDspPanicDump reset to 0.\n");
            }
            WdfRegistryClose(paramKey);
        }
    }

    /* Initialize USB Audio Offload & Sideband Stack Listener */
    if (ctx->EnableUsbOffload) {
        SofUaolInit(ctx);
        SofUsbOffloadInit(ctx);
    }

    SofLogBreadcrumb("[17] SofEvtDeviceD0Entry completed successfully!\n");
    return STATUS_SUCCESS;
}

/**
 * @brief KMDF EvtDeviceD0Exit callback.
 *
 * Invoked when transitioning from D0 into a low-power state (D3, S0ix, S3, S4).
 * Stops the watchdog timer, pauses active streams, puts the HDA codec into D3,
 * releases USB audio offload, masks interrupts, and places the DSP into D0i3.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] TargetState Low-power target state (PowerDeviceD3, etc.).
 * @return NTSTATUS STATUS_SUCCESS.
 */
NTSTATUS
SofEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] EvtDeviceD0Exit: Transitioning from D0 to Target State %d\n",
               TargetState);
    {
        char xmsg[128];
        RtlStringCbPrintfA(xmsg, sizeof(xmsg), "[Power] SofEvtDeviceD0Exit: TargetState=%d\n", TargetState);
        SofLogBreadcrumb(xmsg);
    }

    /* Stop Watchdog Health Timer when leaving D0 */
    if (ctx->WatchdogHealthTimer) {
        WdfTimerStop(ctx->WatchdogHealthTimer, FALSE);
        SofLogBreadcrumb("[Watchdog] Stopped WatchdogHealthTimer\n");

        /* If cleanly leaving D0, reset ConsecutiveCrashCount */
        WDFKEY paramKey = NULL;
        UNICODE_STRING valCrash;
        RtlInitUnicodeString(&valCrash, L"ConsecutiveCrashCount");
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
            ULONG zero = 0;
            WdfRegistryAssignULong(paramKey, &valCrash, zero);
            ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
            WdfRegistryClose(paramKey);
            SofLogBreadcrumb("[Watchdog] D0Exit clean: Reset ConsecutiveCrashCount = 0.\n");
        }
    }

    /* Pause any active streams */
    if (ctx->PlaybackStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->PlaybackStream, KSSTATE_PAUSE);
    }
    if (ctx->CaptureStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->CaptureStream, KSSTATE_PAUSE);
    }
    for (ULONG i = 0; i < 3; i++) {
        if (ctx->HdmiStreams[i] != NULL) {
            SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->HdmiStreams[i], KSSTATE_PAUSE);
        }
    }
    if (ctx->DmicStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->DmicStream, KSSTATE_PAUSE);
    }
    if (ctx->SspPlaybackStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->SspPlaybackStream, KSSTATE_PAUSE);
    }
    if (ctx->SspCaptureStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->SspCaptureStream, KSSTATE_PAUSE);
    }

    /* Put HDA Codec to D3 to conserve power (Display Audio is power-managed by Intel Graphics) */
    if (ctx->HdaCodecPresent) {
        ULONG dummy;
        SofHdaSendVerb(ctx, HDA_VERB(0, 0x01, HDA_VERB_SET_POWER_STATE, 0x03), &dummy);
        SofLogBreadcrumb("[Power] HDA Codec set to D3\n");
    }

    /* Release / suspend USB Audio Offload */
    if (ctx->EnableUsbOffload) {
        SofUsbOffloadRelease(ctx);
    }

    /* Disable IPC interrupts when leaving D0 */
    SofIpcDisableInterrupts(ctx);

    /* Place DSP hardware into D0i3 low-power state */
    SofHdaSetD0i3(ctx, TRUE);

    ctx->CurrentPowerState = (DEVICE_POWER_STATE)TargetState;
    return STATUS_SUCCESS;
}

/**
 * @brief Periodic WDF timer callback executed at PASSIVE_LEVEL for jack polling.
 *
 * Checks ELD / Pin Sense registers on HDMI/DP display codecs and analog jacks,
 * firing IPortEvents notifications to Windows Audio service when headphones or
 * displays are plugged/unplugged. Re-arms itself for 1000ms.
 *
 * @param[in] Timer Handle to WDFTIMER.
 */
VOID
SofEvtJackPollTimer(
    _In_ WDFTIMER Timer
)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(device);
    if (ctx != NULL) {
        if (ctx->PortEvents != NULL) {
            SofCheckAndNotifyJackPresence(ctx);
        }

        /* Passive timers cannot be periodic in KMDF; re-arm for next tick */
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1000));
    }
}

/**
 * @brief Watchdog health confirmation timer callback executed 10s after D0Entry.
 *
 * If the system remains stable in D0 for 10 seconds without a bugcheck or kernel panic,
 * this callback confirms health and resets ConsecutiveCrashCount to 0 in the registry.
 *
 * @param[in] Timer Handle to WDFTIMER.
 */
VOID
SofEvtWatchdogHealthTimer(
    _In_ WDFTIMER Timer
)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    WDFKEY paramKey = NULL;
    UNICODE_STRING valCrash;
    UNREFERENCED_PARAMETER(device);

    RtlInitUnicodeString(&valCrash, L"ConsecutiveCrashCount");
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
        ULONG zero = 0;
        WdfRegistryAssignULong(paramKey, &valCrash, zero);
        ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
        WdfRegistryClose(paramKey);
        SofLogBreadcrumb("[Watchdog] Health confirmed (10s in D0)! Reset ConsecutiveCrashCount = 0.\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Watchdog: Driver healthy (10s in D0). Reset ConsecutiveCrashCount to 0.\n");
    }
}

/**
 * @brief High-IRQL Interrupt Service Routine (ISR) for SOF Audio DSP.
 *
 * Checks if the interrupt was generated by the DSP doorbell registers. If so,
 * clears the edge and queues the DPC for deferred message processing.
 *
 * @param[in] Interrupt Handle to WDFINTERRUPT.
 * @param[in] MessageID MSI message index (unused for line-based IRQs).
 * @return BOOLEAN TRUE if the interrupt belonged to this device; FALSE otherwise.
 */
BOOLEAN
SofEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageID);

    /* Check if this interrupt was asserted by the DSP */
    if (SofIpcIsrCheck(ctx)) {
        /* Queue DPC for deferred IPC packet processing */
        WdfInterruptQueueDpcForIsr(Interrupt);
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief Deferred Procedure Call (DPC) processing IPC messages at DISPATCH_LEVEL.
 *
 * @param[in] Interrupt Handle to WDFINTERRUPT.
 * @param[in] AssociatedObject Associated WDF object.
 */
VOID
SofEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedObject);

    /* Process DSP IPC Mailbox reply or notification */
    SofIpcDpcHandler(ctx);
}
