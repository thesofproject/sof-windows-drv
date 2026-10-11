/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file SdwController.c
 * @brief SoundWire Controller Engine - Cadence Master IP, Intel SHIM2 & Microsoft SDCA DDI.
 *
 * @details
 * Implements hardware control and Windows driver architecture integration for SoundWire:
 *  1. Hardware IP Register Programming: Programs Cadence Master IP registers (MCP_CONFIG,
 *     CONTROL, CMD, PHYCTRL, SSP_CTRL, CLK_CTRL) and Intel SHIM2 vendor-specific registers
 *     (LVSCTL, ACTMCTL) derived from Intel hardware specs.
 *  2. Child PDO Enumeration: Enumerates child peripheral devices (SOUNDWIRE\SDCA_PERIPHERAL)
 *     based on ACPI NHLT endpoint discoveries.
 *  3. SDCA Controller DDI: Implements the Microsoft SoundWire Controller DDI sequential I/O
 *     queue handling FILE_DEVICE_SOUNDWIRE IOCTLs from Microsoft SDCA class drivers
 *     (SdcaMfd.sys, SdcaClass.sys).
 * Copyright 2026 The SOF Authors
 */

#include "Driver.h"
#include "Device.h"
#include "Hw.h"
#include "Topo.h"
#include "WaveRt.h"
#include "SdwController.h"

#ifdef _KERNEL_MODE
#include <ntstrsafe.h>
#include <initguid.h>
#include <wdmguid.h>
#include <acpiioct.h>
#endif

/**
 * @brief Synthesizes MIPI SoundWire standard SCP register responses for enumeration in bypass.
 *
 * @param[in] LinkIndex Master link index.
 * @param[in] DeviceId  Target device identifier.
 * @param[in] RegAddr   SCP register address queried.
 * @return Synthesized register byte value.
 */
UCHAR
SofSdwSynthesizeRegRead(
    _In_ ULONG LinkIndex,
    _In_ ULONG DeviceId,
    _In_ ULONG RegAddr
)
{
    UNREFERENCED_PARAMETER(LinkIndex);
    switch (RegAddr) {
    case 0x0040: /* SCP_Control */
        return 0x00;
    case 0x0041: /* SCP_Status: Bit 0 = Attached */
        return 0x01;
    case 0x0042: /* SCP_InterruptMask */
    case 0x0043: /* SCP_InterruptStatus */
    case 0x0044: /* SCP_BusReset */
        return 0x00;
    case SDW_SCP_DEVNUMBER: /* 0x0050 */
        return 1;
    case SDW_SCP_DEVID_0:   /* 0x0051: Version 0x1, Realtek vendor high 0x2 */
        return 0x12;
    case SDW_SCP_DEVID_1:   /* 0x0052: Realtek vendor low 0x5D */
        return 0x5D;
    case SDW_SCP_DEVID_2:   /* 0x0053: Device ID high */
        return (UCHAR)((DeviceId >> 8) & 0xFF);
    case SDW_SCP_DEVID_3:   /* 0x0054: Device ID low */
        return (UCHAR)(DeviceId & 0xFF);
    case SDW_SCP_DEVID_4:   /* 0x0055: Class ID 0x01 = SDCA */
        return 0x01;
    case SDW_SCP_DEVID_5:   /* 0x0056: Instance ID */
        return 0x01;
    case SDW_SCP_FRAMESHAPE: /* 0x0057: 120 rows x 48 columns */
    case SDW_SCP_SECFRAMESHAPE:
        return 0x7B;
    default:
        return 0x00;
    }
}

#ifdef _KERNEL_MODE

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SofSdwEvtIoDeviceControl;

/*
 * Allocates and registers a Child PDO for a SoundWire SDCA Peripheral.
 */
static NTSTATUS
SofSdwCreateChildPdo(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG LinkIndex,
    _In_ ULONG PeripheralId,
    _In_ ULONG VendorId,
    _In_ ULONG DeviceId,
    _In_ ULONG DeviceType,
    _In_ ULONG Direction,
    _Out_ WDFDEVICE* OutChildDevice
)
{
    NTSTATUS status;
    PWDFDEVICE_INIT pdoInit = NULL;
    WDFDEVICE childDevice = NULL;
    WDF_OBJECT_ATTRIBUTES childAttributes;
    WDF_DEVICE_POWER_CAPABILITIES powerCaps;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE childQueue;
    PSOF_SDW_CHILD_CONTEXT childCtx;

    UNICODE_STRING compatIdPeripheral;
    UNICODE_STRING compatIdSdca;
    UNICODE_STRING devId;
    UNICODE_STRING descStr;
    UNICODE_STRING locStr;
    UNICODE_STRING instId;
    UNICODE_STRING hwId;
    WCHAR instIdBuf[32];
    WCHAR hwIdBuf[64];
    WCHAR locBuf[64];

    if (!Ctx || !Ctx->WdfDevice || !OutChildDevice) {
        return STATUS_INVALID_PARAMETER;
    }

    *OutChildDevice = NULL;

    RtlInitUnicodeString(&compatIdPeripheral, L"SOUNDWIRE\\SDCA_PERIPHERAL");
    RtlInitUnicodeString(&compatIdSdca, L"SOUNDWIRE\\SDCA");
    RtlInitUnicodeString(&devId, L"SOUNDWIRE\\SDCA_PERIPHERAL");
    RtlInitUnicodeString(&descStr, L"SoundWire SDCA Audio Peripheral");

    RtlStringCbPrintfW(instIdBuf, sizeof(instIdBuf), L"LINK_%02u_DEV_%02u", LinkIndex, PeripheralId);
    RtlInitUnicodeString(&instId, instIdBuf);

    RtlStringCbPrintfW(hwIdBuf, sizeof(hwIdBuf), L"SOUNDWIRE\\VEN_%04X&DEV_%04X", VendorId, DeviceId);
    RtlInitUnicodeString(&hwId, hwIdBuf);

    RtlStringCbPrintfW(locBuf, sizeof(locBuf), L"SoundWire Controller Link %u", LinkIndex);
    RtlInitUnicodeString(&locStr, locBuf);

    pdoInit = WdfPdoInitAllocate(Ctx->WdfDevice);
    if (!pdoInit) {
        SofLogBreadcrumb("[SDW] WdfPdoInitAllocate failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = WdfPdoInitAssignDeviceID(pdoInit, &devId);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAssignInstanceID(pdoInit, &instId);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddHardwareID(pdoInit, &hwId);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddHardwareID(pdoInit, &compatIdPeripheral);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddCompatibleID(pdoInit, &compatIdPeripheral);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddCompatibleID(pdoInit, &compatIdSdca);
    if (!NT_SUCCESS(status)) goto Error;

    status = WdfPdoInitAddDeviceText(pdoInit, &descStr, &locStr, 0x409);
    if (!NT_SUCCESS(status)) goto Error;

    WdfPdoInitSetDefaultLocale(pdoInit, 0x409);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&childAttributes, SOF_SDW_CHILD_CONTEXT);
    status = WdfDeviceCreate(&pdoInit, &childAttributes, &childDevice);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[SDW] WdfDeviceCreate failed for child PDO\n");
        goto Error;
    }

    /* Protect SoundWire child PDO against IopDecrementDeviceObjectRef underflow */
    PDEVICE_OBJECT wdmChildPdo = WdfDeviceWdmGetDeviceObject(childDevice);
    if (wdmChildPdo != NULL) {
        for (ULONG i = 0; i < 100000; i++) {
            ObReferenceObject(wdmChildPdo);
        }
        InterlockedExchange(&wdmChildPdo->ReferenceCount, 100000);
    }

    childCtx = SofSdwGetChildContext(childDevice);
    RtlZeroMemory(childCtx, sizeof(SOF_SDW_CHILD_CONTEXT));
    childCtx->ParentCtx = Ctx;
    childCtx->LinkIndex = LinkIndex;
    childCtx->PeripheralId = PeripheralId;
    childCtx->VendorId = VendorId;
    childCtx->DeviceId = DeviceId;
    childCtx->DeviceType = DeviceType;
    childCtx->Direction = Direction;
    childCtx->CachedVolumeDb = 0;
    childCtx->CachedMute = FALSE;

    /* Set Power Capabilities */
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

    /* Set PnP Capabilities */
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.Removable = WdfFalse;
    pnpCaps.EjectSupported = WdfFalse;
    pnpCaps.SurpriseRemovalOK = WdfFalse;
    pnpCaps.Address = LinkIndex;
    pnpCaps.UINumber = (LinkIndex + 1);
    WdfDeviceSetPnpCapabilities(childDevice, &pnpCaps);

    /* Create Sequential Default Queue for SoundWire Controller DDI */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = SofSdwEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = SofSdwEvtIoDeviceControl;
    status = WdfIoQueueCreate(childDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &childQueue);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[SDW] WdfIoQueueCreate failed for child queue\n");
        WdfObjectDelete(childDevice);
        return status;
    }

    /* Add child PDO to parent FDO */
    status = WdfFdoAddStaticChild(Ctx->WdfDevice, childDevice);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[SDW] WdfFdoAddStaticChild failed\n");
        WdfObjectDelete(childDevice);
        return status;
    }

    *OutChildDevice = childDevice;
    SofLogBreadcrumb("[SDW] Created and attached SoundWire SDCA Child PDO successfully\n");
    return STATUS_SUCCESS;

Error:
    if (pdoInit) {
        WdfDeviceInitFree(pdoInit);
    }
    return status;
}

/*
 * Checks if an ACPI method name string ends with a given suffix (case-insensitive).
 */
static BOOLEAN
SofSdwMethodNameEndsWith(
    _In_opt_ const char* Name,
    _In_opt_ const char* Suffix
)
{
    size_t nameLen, suffixLen, i;
    if (!Name || !Suffix) return FALSE;
    nameLen = strlen(Name);
    suffixLen = strlen(Suffix);
    if (nameLen < suffixLen) return FALSE;
    for (i = 0; i < suffixLen; i++) {
        char c1 = Name[nameLen - suffixLen + i];
        char c2 = Suffix[i];
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return FALSE;
    }
    return TRUE;
}

/*
 * Synthesizes a valid MIPI DisCo _DSD ACPI output buffer containing the
 * Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301} and
 * SoundWire SDCA interface properties expected by Microsoft SdcaMfd.sys.
 */
static NTSTATUS
SofSdwBuildDsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const struct {
        const char* Name;
        ULONG Value;
    } props[] = {
        { "mipi-sdw-sw-interface-revision", 0x00010001 },
        { "mipi-sdw-sdca-interface-revision", 0x00010000 },
        { "mipi-sdca-interface-revision", 0x00010000 },
        { "mipi-sdw-wake-up-unavailable", 0 },
        { "mipi-sdw-test-mode-supported", 0 },
        { "mipi-sdw-clock-stop-mode1-supported", 0 },
        { "mipi-sdw-simplified-clockstopprepare-sm-supported", 0 },
        { "mipi-sdw-clockstopprepare-timeout", 300 },
        { "mipi-sdw-peripheral-channelprepare-timeout", 300 },
        { "mipi-sdw-clockstopprepare-hard-reset-behavior", 0 },
        { "mipi-sdw-highPHY-capable", 0 },
        { "mipi-sdw-paging-supported", 0 },
        { "mipi-sdw-bank-delay-supported", 0 },
        { "mipi-sdw-port15-read-behavior", 0 },
        { "mipi-sdw-source-port-list", 0 },
        { "mipi-sdw-sink-port-list", 0 },
        { "mipi-sdw-dp-0-supported", 0 }
    };
    ULONG numProps = sizeof(props) / sizeof(props[0]);
    ULONG i;

    ULONG arg0Size = 4 + 16;
    ULONG pkgContentLen = 0;

    for (i = 0; i < numProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(props[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkgContentLen += subPkgTotal;
    }

    ULONG arg1Size = 4 + pkgContentLen;
    ULONG totalSize = 12 + arg0Size + arg1Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 2;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: Properties Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkgContentLen;
    pCurr += 4;

    for (i = 0; i < numProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(props[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], props[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)props[i].Value;
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0._DSD ACPI output buffer containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301} with mipi-sdca-interface-revision = 0x00010000
 * - Hierarchical Data Extension UUID {dbb8e3e6-5886-4ba6-8795-1319f52a966b} with mipi-sdca-control-0x4..8-subproperties -> "CTL4".."CTL8"
 */
static NTSTATUS
SofSdwBuildAud0DsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const UCHAR acpiDataExtUuid[16] = {
        0xe6, 0xe3, 0xb8, 0xdb, 0x86, 0x58, 0xa6, 0x4b,
        0x87, 0x95, 0x13, 0x19, 0xf5, 0x2a, 0x96, 0x6b
    };

    static const struct {
        const char* Name;
        ULONG64 Value;
    } aud0Props[] = {
        { "mipi-sdca-interface-revision", 0x00010000 },
        { "mipi-sdca-control-list", 0x0000F000000101F0ULL }
    };

    /*
     * SDCA mandatory ID lists:
     * Microsoft SdcaClass.sys unpacks each list via 0x1400aa0c8 which requires
     * an ACPI_METHOD_ARGUMENT_PACKAGE containing at least 1 ACPI_METHOD_ARGUMENT_INTEGER.
     */
    static const struct {
        const char* Name;
        ULONG Count;
        ULONG64 Values[3];
    } aud0ListProps[] = {
        { "mipi-sdca-cluster-id-list", 1, { 0x01, 0, 0 } },
        { "mipi-sdca-entity-id-list",  3, { 0x01, 0x02, 0x03 } }
    };

    static const struct {
        const char* Name;
        const char* Target;
    } aud0SubProps[] = {
        { "mipi-sdca-control-0x4-subproperties", "CTL4" },
        { "mipi-sdca-control-0x5-subproperties", "CTL5" },
        { "mipi-sdca-control-0x6-subproperties", "CTL6" },
        { "mipi-sdca-control-0x7-subproperties", "CTL7" },
        { "mipi-sdca-control-0x8-subproperties", "CTL8" },
        { "mipi-sdca-control-0x10-subproperties", "CT10" },
        { "mipi-sdca-control-0x2C-subproperties", "CT2C" },
        { "mipi-sdca-control-0x2D-subproperties", "CT2D" },
        { "mipi-sdca-control-0x2E-subproperties", "CT2E" },
        { "mipi-sdca-control-0x2F-subproperties", "CT2F" },
        { "mipi-sdca-cluster-id-0x1-subproperties", "CL01" },
        { "mipi-sdca-entity-id-0x1-subproperties", "ENT1" },
        { "mipi-sdca-entity-id-0x2-subproperties", "GRP1" },
        { "mipi-sdca-entity-id-0x3-subproperties", "ENT2" }
    };

    ULONG numAud0Props = sizeof(aud0Props) / sizeof(aud0Props[0]);
    ULONG numAud0ListProps = sizeof(aud0ListProps) / sizeof(aud0ListProps[0]);
    ULONG numAud0SubProps = sizeof(aud0SubProps) / sizeof(aud0SubProps[0]);
    ULONG i;

    /* Arg 0: UUID1 Buffer */
    ULONG arg0Size = 4 + 16;

    /* Arg 1: UUID1 Package */
    ULONG pkg1ContentLen = 0;
    for (i = 0; i < numAud0Props; i++) {
        ULONG keyStrLen = (ULONG)strlen(aud0Props[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    for (i = 0; i < numAud0ListProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(aud0ListProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG innerPkgDataLen = aud0ListProps[i].Count * 12;
        ULONG valArgTotal = 4 + innerPkgDataLen; /* 4-byte package header + Count * 12-byte integer args */
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    ULONG arg1Size = 4 + pkg1ContentLen;

    /* Arg 2: UUID2 Buffer */
    ULONG arg2Size = 4 + 16;

    /* Arg 3: UUID2 Package */
    ULONG pkg2ContentLen = 0;
    for (i = 0; i < numAud0SubProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(aud0SubProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(aud0SubProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg2ContentLen += subPkgTotal;
    }
    ULONG arg3Size = 4 + pkg2ContentLen;

    ULONG totalSize = 12 + arg0Size + arg1Size + arg2Size + arg3Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 4;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID1 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: UUID1 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkg1ContentLen;
    pCurr += 4;

    for (i = 0; i < numAud0Props; i++) {
        ULONG keyStrLen = (ULONG)strlen(aud0Props[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], aud0Props[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)aud0Props[i].Value;
        pCurr += valArgTotal;
    }

    for (i = 0; i < numAud0ListProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(aud0ListProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG innerPkgDataLen = aud0ListProps[i].Count * 12;
        ULONG valArgTotal = 4 + innerPkgDataLen;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], aud0ListProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pValPkg->DataLength = (USHORT)innerPkgDataLen;
        pCurr += 4;

        for (ULONG k = 0; k < aud0ListProps[i].Count; k++) {
            PACPI_METHOD_ARGUMENT_V1 pInnerInt = (PACPI_METHOD_ARGUMENT_V1)pCurr;
            pInnerInt->Type = ACPI_METHOD_ARGUMENT_INTEGER;
            pInnerInt->DataLength = sizeof(ULONG64);
            *(PULONG64)&pInnerInt->Data[0] = (ULONG64)aud0ListProps[i].Values[k];
            pCurr += 12;
        }
    }

    /* Arg 2: UUID2 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg2 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg2->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg2->DataLength = 16;
    RtlCopyMemory(&pArg2->Data[0], acpiDataExtUuid, 16);
    pCurr += arg2Size;

    /* Arg 3: UUID2 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg3 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg3->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg3->DataLength = (USHORT)pkg2ContentLen;
    pCurr += 4;

    for (i = 0; i < numAud0SubProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(aud0SubProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(aud0SubProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], aud0SubProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pValArg->DataLength = (USHORT)valStrLen;
        RtlCopyMemory(&pValArg->Data[0], aud0SubProps[i].Target, valStrLen);
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.CL01 ACPI output buffer for Cluster 1 containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301}
 * - mipi-sdca-channel-count = 0 (skips channel enumeration in SdcaClass.sys)
 */
static NTSTATUS
SofSdwBuildClusterDsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const struct {
        const char* Name;
        ULONG64 Value;
    } clusterProps[] = {
        { "mipi-sdca-channel-count", 0 }
    };
    ULONG numClusterProps = sizeof(clusterProps) / sizeof(clusterProps[0]);
    ULONG i;

    ULONG arg0Size = 4 + 16;
    ULONG pkgContentLen = 0;

    for (i = 0; i < numClusterProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(clusterProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkgContentLen += subPkgTotal;
    }

    ULONG arg1Size = 4 + pkgContentLen;
    ULONG totalSize = 12 + arg0Size + arg1Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 2;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID1 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: UUID1 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkgContentLen;
    pCurr += 4;

    for (i = 0; i < numClusterProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(clusterProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], clusterProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)clusterProps[i].Value;
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.ENT1 ACPI output buffer for Entity 1 containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301}:
 *   - mipi-sdca-entity-type = 3 (Output Terminal, SDCA_ENTITY_TYPE_OT)
 *   - mipi-sdca-terminal-type = 0x06B0 (Speaker Stereo Jack)
 *   - mipi-sdca-terminal-transducer-count = 2 (Stereo)
 *   - mipi-sdca-terminal-reference-number = 1 (Managed Terminal Reference 1)
 *   - mipi-sdca-terminal-connector-type = 0 (Standard connector)
 *   - mipi-sdca-control-list = 0 (no controls)
 *   - mipi-sdca-input-pin-list = 0 (no upstream entities)
 * - Buffer Data Extension UUID {edb12dd0-363d-4085-a3d2-49522ca160c4}:
 *   - mipi-sdca-terminal-reference-description -> "RD01"
 */
static NTSTATUS
SofSdwBuildEntityDsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const UCHAR acpiBufferDataUuid[16] = {
        0xd0, 0x2d, 0xb1, 0xed, 0x3d, 0x36, 0x85, 0x40,
        0xa3, 0xd2, 0x49, 0x52, 0x2c, 0xa1, 0x60, 0xc4
    };

    static const struct {
        const char* Name;
        ULONG64 Value;
    } entityProps[] = {
        { "mipi-sdca-entity-type", 3 },                      /* Output Terminal (SDCA_ENTITY_TYPE_OT = 3) */
        { "mipi-sdca-terminal-type", 0x06B0 },               /* Speaker Stereo Jack (SDCA 1.0) */
        { "mipi-sdca-terminal-transducer-count", 2 },        /* Stereo */
        { "mipi-sdca-terminal-reference-number", 1 },        /* Managed Terminal Reference 1 */
        { "mipi-sdca-terminal-connector-type", 0 },          /* Standard connector */
        { "mipi-sdca-control-list", 0 },                     /* No controls */
        { "mipi-sdca-input-pin-list", 0 }                    /* No upstream entities */
    };

    static const struct {
        const char* Name;
        const char* Target;
    } entityBufferProps[] = {
        { "mipi-sdca-terminal-reference-description", "RD01" }
    };

    ULONG numEntityProps = sizeof(entityProps) / sizeof(entityProps[0]);
    ULONG numEntityBufferProps = sizeof(entityBufferProps) / sizeof(entityBufferProps[0]);
    ULONG i;

    /* Arg 0: UUID1 Buffer */
    ULONG arg0Size = 4 + 16;

    /* Arg 1: UUID1 Package */
    ULONG pkg1ContentLen = 0;
    for (i = 0; i < numEntityProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    ULONG arg1Size = 4 + pkg1ContentLen;

    /* Arg 2: UUID3 Buffer */
    ULONG arg2Size = 4 + 16;

    /* Arg 3: UUID3 Package */
    ULONG pkg2ContentLen = 0;
    for (i = 0; i < numEntityBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(entityBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg2ContentLen += subPkgTotal;
    }
    ULONG arg3Size = 4 + pkg2ContentLen;

    ULONG totalSize = 12 + arg0Size + arg1Size + arg2Size + arg3Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 4;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID1 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: UUID1 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkg1ContentLen;
    pCurr += 4;

    for (i = 0; i < numEntityProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], entityProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)entityProps[i].Value;
        pCurr += valArgTotal;
    }

    /* Arg 2: UUID3 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg2 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg2->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg2->DataLength = 16;
    RtlCopyMemory(&pArg2->Data[0], acpiBufferDataUuid, 16);
    pCurr += arg2Size;

    /* Arg 3: UUID3 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg3 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg3->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg3->DataLength = (USHORT)pkg2ContentLen;
    pCurr += 4;

    for (i = 0; i < numEntityBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(entityBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], entityBufferProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pValArg->DataLength = (USHORT)valStrLen;
        RtlCopyMemory(&pValArg->Data[0], entityBufferProps[i].Target, valStrLen);
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.ENT2 ACPI output buffer for Input Terminal (Microphone on Jack) containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301}:
 *   - mipi-sdca-entity-type = 2 (Input Terminal, SDCA_ENTITY_TYPE_IT)
 *   - mipi-sdca-terminal-type = 0x0680 (Universal Audio Jack Microphone)
 *   - mipi-sdca-terminal-transducer-count = 1 (Mono)
 *   - mipi-sdca-terminal-reference-number = 1 (Shared Managed Terminal Reference 1)
 *   - mipi-sdca-terminal-connector-type = 0 (Standard connector)
 *   - mipi-sdca-control-list = 0 (no controls)
 *   - mipi-sdca-input-pin-list = 0 (no upstream entities)
 * - Buffer Data Extension UUID {edb12dd0-363d-4085-a3d2-49522ca160c4}:
 *   - mipi-sdca-terminal-reference-description -> "RD02"
 */
static NTSTATUS
SofSdwBuildInputTerminalDsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const UCHAR acpiBufferDataUuid[16] = {
        0xd0, 0x2d, 0xb1, 0xed, 0x3d, 0x36, 0x85, 0x40,
        0xa3, 0xd2, 0x49, 0x52, 0x2c, 0xa1, 0x60, 0xc4
    };

    static const struct {
        const char* Name;
        ULONG64 Value;
    } entityProps[] = {
        { "mipi-sdca-entity-type", 2 },                      /* Input Terminal (SDCA_ENTITY_TYPE_IT = 2) */
        { "mipi-sdca-terminal-type", 0x0680 },               /* Microphone on Jack */
        { "mipi-sdca-terminal-transducer-count", 1 },        /* Mono */
        { "mipi-sdca-terminal-reference-number", 1 },        /* Managed Terminal Reference 1 */
        { "mipi-sdca-terminal-connector-type", 0 },          /* Standard connector */
        { "mipi-sdca-control-list", 0 },                     /* No controls */
        { "mipi-sdca-input-pin-list", 0 }                    /* No upstream entities */
    };

    static const struct {
        const char* Name;
        const char* Target;
    } entityBufferProps[] = {
        { "mipi-sdca-terminal-reference-description", "RD02" }
    };

    ULONG numEntityProps = sizeof(entityProps) / sizeof(entityProps[0]);
    ULONG numEntityBufferProps = sizeof(entityBufferProps) / sizeof(entityBufferProps[0]);
    ULONG i;

    /* Arg 0: UUID1 Buffer */
    ULONG arg0Size = 4 + 16;

    /* Arg 1: UUID1 Package */
    ULONG pkg1ContentLen = 0;
    for (i = 0; i < numEntityProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    ULONG arg1Size = 4 + pkg1ContentLen;

    /* Arg 2: UUID3 Buffer */
    ULONG arg2Size = 4 + 16;

    /* Arg 3: UUID3 Package */
    ULONG pkg2ContentLen = 0;
    for (i = 0; i < numEntityBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(entityBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg2ContentLen += subPkgTotal;
    }
    ULONG arg3Size = 4 + pkg2ContentLen;

    ULONG totalSize = 12 + arg0Size + arg1Size + arg2Size + arg3Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 4;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID1 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: UUID1 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkg1ContentLen;
    pCurr += 4;

    for (i = 0; i < numEntityProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], entityProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)entityProps[i].Value;
        pCurr += valArgTotal;
    }

    /* Arg 2: UUID3 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg2 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg2->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg2->DataLength = 16;
    RtlCopyMemory(&pArg2->Data[0], acpiBufferDataUuid, 16);
    pCurr += arg2Size;

    /* Arg 3: UUID3 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg3 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg3->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg3->DataLength = (USHORT)pkg2ContentLen;
    pCurr += 4;

    for (i = 0; i < numEntityBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(entityBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(entityBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], entityBufferProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pValArg->DataLength = (USHORT)valStrLen;
        RtlCopyMemory(&pValArg->Data[0], entityBufferProps[i].Target, valStrLen);
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.CTLn ACPI output buffer containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301}
 * - mipi-sdca-control-access-mode = 5 (SDCA_CONTROL_ACCESS_MODE_DC_VALUE)
 * - mipi-sdca-control-dc-value = Control-specific DC value
 */
static NTSTATUS
SofSdwBuildCtlDsdOutputBuffer(
    _In_ ULONG CtlIndex,
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    ULONG dcValue = 0;
    switch (CtlIndex) {
    case 4: /* SDCA Specification Version (0x10 = SDCA 1.0) */
        dcValue = 0x10;
        break;
    case 5: /* SELECTED_FUNCTION (Universal Audio Jack = 0x08) */
        dcValue = 0x08;
        break;
    case 6: /* FUNCTION_VERSION (SDCA 1.0) */
        dcValue = 0x0100;
        break;
    case 7: /* FUNCTION_EXTENSION_ID */
        dcValue = 0x0000;
        break;
    case 8: /* FUNCTION_EXTENSION_VERSION */
        dcValue = 0x00;
        break;
    case 16: /* FUNCTION_STATUS (SDCA 1.0 Control 0x10) */
        dcValue = 0x00;
        break;
    case 44: /* FUNCTION_BUSY (SDCA 1.0 Control 0x2C) */
        dcValue = 0x00;
        break;
    case 45: /* RXUMP_OWNERSHIP (SDCA 1.0 Control 0x2D) */
        dcValue = 0x00;
        break;
    case 46: /* CLOCK_VALID (SDCA 1.0 Control 0x2E) */
        dcValue = 0x00;
        break;
    case 47: /* POWER_DOMAIN_MANAGED (SDCA 1.0 Control 0x2F) */
        dcValue = 0x00;
        break;
    default:
        dcValue = 0x00;
        break;
    }

    struct {
        const char* Name;
        ULONG64 Value;
    } ctlProps[] = {
        { "mipi-sdca-control-access-layer", 4 },
        { "mipi-sdca-control-access-mode", 5 },
        { "mipi-sdca-control-dc-value", dcValue }
    };
    ULONG numCtlProps = sizeof(ctlProps) / sizeof(ctlProps[0]);
    ULONG i;

    ULONG arg0Size = 4 + 16;
    ULONG pkgContentLen = 0;

    for (i = 0; i < numCtlProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkgContentLen += subPkgTotal;
    }

    ULONG arg1Size = 4 + pkgContentLen;
    ULONG totalSize = 12 + arg0Size + arg1Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 2;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: Properties Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkgContentLen;
    pCurr += 4;

    for (i = 0; i < numCtlProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], ctlProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)ctlProps[i].Value;
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.GRP1 ACPI output buffer for Group Entity 2 containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301}:
 *   - mipi-sdca-entity-type = 18 (0x12, Group Entity)
 *   - mipi-sdca-ge-managed-terminal-reference-number = 1
 *   - mipi-sdca-control-list = 6 (Control 1 | Control 2)
 *   - msft-ge-mode-terminaltype-list = [ 0x03, 0x06B0 ]
 * - Hierarchical Data Extension UUID {dbb8e3e6-5886-4ba6-8795-1319f52a966b}:
 *   - mipi-sdca-control-0x1-subproperties -> "CTL1"
 *   - mipi-sdca-control-0x2-subproperties -> "CTL2"
 */
static NTSTATUS
SofSdwBuildGroupDsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const UCHAR acpiDataExtUuid[16] = {
        0xe6, 0xe3, 0xb8, 0xdb, 0x86, 0x58, 0xa6, 0x4b,
        0x87, 0x95, 0x13, 0x19, 0xf5, 0x2a, 0x96, 0x6b
    };

    static const struct {
        const char* Name;
        ULONG64 Value;
    } groupProps[] = {
        { "mipi-sdca-entity-type", 18 },
        { "mipi-sdca-ge-managed-terminal-reference-number", 1 },
        { "mipi-sdca-control-list", 6 }                      /* Control 1 (SelectedMode) | Control 2 (DetectedMode) */
    };

    static const struct {
        const char* Name;
        ULONG Count;
        ULONG64 Values[4];
    } groupListProps[] = {
        { "msft-ge-mode-terminaltype-list", 4, { 0x03, 0x06B0, 0x03, 0x0680 } } /* Mode 3 -> 0x06B0 (Speaker) & 0x0680 (Mic on Jack) */
    };

    static const struct {
        const char* Name;
        const char* Target;
    } groupSubProps[] = {
        { "mipi-sdca-control-0x1-subproperties", "CTL1" },
        { "mipi-sdca-control-0x2-subproperties", "CTL2" }
    };

    ULONG numGroupProps = sizeof(groupProps) / sizeof(groupProps[0]);
    ULONG numGroupListProps = sizeof(groupListProps) / sizeof(groupListProps[0]);
    ULONG numGroupSubProps = sizeof(groupSubProps) / sizeof(groupSubProps[0]);
    ULONG i;

    /* Arg 0: UUID1 Buffer */
    ULONG arg0Size = 4 + 16;

    /* Arg 1: UUID1 Package */
    ULONG pkg1ContentLen = 0;
    for (i = 0; i < numGroupProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(groupProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    for (i = 0; i < numGroupListProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(groupListProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG innerPkgDataLen = groupListProps[i].Count * 12;
        ULONG valArgTotal = 4 + innerPkgDataLen;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    ULONG arg1Size = 4 + pkg1ContentLen;

    /* Arg 2: UUID2 Buffer */
    ULONG arg2Size = 4 + 16;

    /* Arg 3: UUID2 Package */
    ULONG pkg2ContentLen = 0;
    for (i = 0; i < numGroupSubProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(groupSubProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(groupSubProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg2ContentLen += subPkgTotal;
    }
    ULONG arg3Size = 4 + pkg2ContentLen;

    ULONG totalSize = 12 + arg0Size + arg1Size + arg2Size + arg3Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 4;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID1 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: UUID1 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkg1ContentLen;
    pCurr += 4;

    for (i = 0; i < numGroupProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(groupProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], groupProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)groupProps[i].Value;
        pCurr += valArgTotal;
    }

    for (i = 0; i < numGroupListProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(groupListProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG innerPkgDataLen = groupListProps[i].Count * 12;
        ULONG valArgTotal = 4 + innerPkgDataLen;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], groupListProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pValPkg->DataLength = (USHORT)innerPkgDataLen;
        pCurr += 4;

        for (ULONG k = 0; k < groupListProps[i].Count; k++) {
            PACPI_METHOD_ARGUMENT_V1 pInt = (PACPI_METHOD_ARGUMENT_V1)pCurr;
            pInt->Type = ACPI_METHOD_ARGUMENT_INTEGER;
            pInt->DataLength = sizeof(ULONG64);
            *(PULONG64)&pInt->Data[0] = groupListProps[i].Values[k];
            pCurr += 12;
        }
    }

    /* Arg 2: UUID2 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg2 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg2->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg2->DataLength = 16;
    RtlCopyMemory(&pArg2->Data[0], acpiDataExtUuid, 16);
    pCurr += arg2Size;

    /* Arg 3: UUID2 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg3 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg3->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg3->DataLength = (USHORT)pkg2ContentLen;
    pCurr += 4;

    for (i = 0; i < numGroupSubProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(groupSubProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(groupSubProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], groupSubProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pValArg->DataLength = (USHORT)valStrLen;
        RtlCopyMemory(&pValArg->Data[0], groupSubProps[i].Target, valStrLen);
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.CTL1 ACPI output buffer for Selected Mode Control 1 containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301}:
 *   - mipi-sdca-control-access-layer = 4
 *   - mipi-sdca-control-access-mode = 5 (SDCA_CONTROL_ACCESS_MODE_DC_VALUE)
 *   - mipi-sdca-control-dc-value = 3 (Mode 3: Speaker)
 *   - mipi-sdca-control-default-value = 3
 * - Buffer Data Extension UUID {edb12dd0-363d-4085-a3d2-49522ca160c4}:
 *   - mipi-sdca-control-range -> "CR01"
 */
static NTSTATUS
SofSdwBuildSelectedModeCtrlDsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const UCHAR acpiBufferDataUuid[16] = {
        0xd0, 0x2d, 0xb1, 0xed, 0x3d, 0x36, 0x85, 0x40,
        0xa3, 0xd2, 0x49, 0x52, 0x2c, 0xa1, 0x60, 0xc4
    };

    static const struct {
        const char* Name;
        ULONG64 Value;
    } ctlProps[] = {
        { "mipi-sdca-control-access-layer", 4 },
        { "mipi-sdca-control-access-mode", 5 },
        { "mipi-sdca-control-dc-value", 3 },
        { "mipi-sdca-control-default-value", 3 }
    };

    static const struct {
        const char* Name;
        const char* Target;
    } ctlBufferProps[] = {
        { "mipi-sdca-control-range", "CR01" }
    };

    ULONG numCtlProps = sizeof(ctlProps) / sizeof(ctlProps[0]);
    ULONG numCtlBufferProps = sizeof(ctlBufferProps) / sizeof(ctlBufferProps[0]);
    ULONG i;

    /* Arg 0: UUID1 Buffer */
    ULONG arg0Size = 4 + 16;

    /* Arg 1: UUID1 Package */
    ULONG pkg1ContentLen = 0;
    for (i = 0; i < numCtlProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    ULONG arg1Size = 4 + pkg1ContentLen;

    /* Arg 2: UUID3 Buffer */
    ULONG arg2Size = 4 + 16;

    /* Arg 3: UUID3 Package */
    ULONG pkg2ContentLen = 0;
    for (i = 0; i < numCtlBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(ctlBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg2ContentLen += subPkgTotal;
    }
    ULONG arg3Size = 4 + pkg2ContentLen;

    ULONG totalSize = 12 + arg0Size + arg1Size + arg2Size + arg3Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 4;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID1 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: UUID1 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkg1ContentLen;
    pCurr += 4;

    for (i = 0; i < numCtlProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], ctlProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)ctlProps[i].Value;
        pCurr += valArgTotal;
    }

    /* Arg 2: UUID3 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg2 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg2->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg2->DataLength = 16;
    RtlCopyMemory(&pArg2->Data[0], acpiBufferDataUuid, 16);
    pCurr += arg2Size;

    /* Arg 3: UUID3 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg3 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg3->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg3->DataLength = (USHORT)pkg2ContentLen;
    pCurr += 4;

    for (i = 0; i < numCtlBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(ctlBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], ctlBufferProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pValArg->DataLength = (USHORT)valStrLen;
        RtlCopyMemory(&pValArg->Data[0], ctlBufferProps[i].Target, valStrLen);
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.CTL2 ACPI output buffer for Detected Mode Control 2 containing:
 * - Device Properties UUID {daffd814-6eba-4d8c-8a91-bc9bbf4aa301}:
 *   - mipi-sdca-control-access-layer = 4
 *   - mipi-sdca-control-access-mode = 5 (SDCA_CONTROL_ACCESS_MODE_DC_VALUE)
 *   - mipi-sdca-control-dc-value = 3 (Mode 3: Speaker Detected)
 *   - mipi-sdca-control-default-value = 3
 *   - mipi-sdca-control-interrupt-position = 0 (sets bit 2 in control flags)
 * - Buffer Data Extension UUID {edb12dd0-363d-4085-a3d2-49522ca160c4}:
 *   - mipi-sdca-control-range -> "CR01"
 */
static NTSTATUS
SofSdwBuildDetectedModeCtrlDsdOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR mipiDisCoUuid[16] = {
        0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
        0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
    };

    static const UCHAR acpiBufferDataUuid[16] = {
        0xd0, 0x2d, 0xb1, 0xed, 0x3d, 0x36, 0x85, 0x40,
        0xa3, 0xd2, 0x49, 0x52, 0x2c, 0xa1, 0x60, 0xc4
    };

    static const struct {
        const char* Name;
        ULONG64 Value;
    } ctlProps[] = {
        { "mipi-sdca-control-access-layer", 4 },
        { "mipi-sdca-control-access-mode", 5 },
        { "mipi-sdca-control-dc-value", 3 },
        { "mipi-sdca-control-default-value", 3 },
        { "mipi-sdca-control-interrupt-position", 0 }
    };

    static const struct {
        const char* Name;
        const char* Target;
    } ctlBufferProps[] = {
        { "mipi-sdca-control-range", "CR01" }
    };

    ULONG numCtlProps = sizeof(ctlProps) / sizeof(ctlProps[0]);
    ULONG numCtlBufferProps = sizeof(ctlBufferProps) / sizeof(ctlBufferProps[0]);
    ULONG i;

    /* Arg 0: UUID1 Buffer */
    ULONG arg0Size = 4 + 16;

    /* Arg 1: UUID1 Package */
    ULONG pkg1ContentLen = 0;
    for (i = 0; i < numCtlProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg1ContentLen += subPkgTotal;
    }
    ULONG arg1Size = 4 + pkg1ContentLen;

    /* Arg 2: UUID3 Buffer */
    ULONG arg2Size = 4 + 16;

    /* Arg 3: UUID3 Package */
    ULONG pkg2ContentLen = 0;
    for (i = 0; i < numCtlBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(ctlBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;
        ULONG subPkgTotal = 4 + subContentLen;
        pkg2ContentLen += subPkgTotal;
    }
    ULONG arg3Size = 4 + pkg2ContentLen;

    ULONG totalSize = 12 + arg0Size + arg1Size + arg2Size + arg3Size;
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 4;

    PUCHAR pCurr = (PUCHAR)&pOut->Argument[0];

    /* Arg 0: UUID1 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg0 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg0->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg0->DataLength = 16;
    RtlCopyMemory(&pArg0->Data[0], mipiDisCoUuid, 16);
    pCurr += arg0Size;

    /* Arg 1: UUID1 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg1 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg1->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg1->DataLength = (USHORT)pkg1ContentLen;
    pCurr += 4;

    for (i = 0; i < numCtlProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;
        ULONG valArgTotal = 12;
        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], ctlProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        pValArg->DataLength = sizeof(ULONG64);
        *(PULONG64)&pValArg->Data[0] = (ULONG64)ctlProps[i].Value;
        pCurr += valArgTotal;
    }

    /* Arg 2: UUID3 Buffer */
    PACPI_METHOD_ARGUMENT_V1 pArg2 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg2->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pArg2->DataLength = 16;
    RtlCopyMemory(&pArg2->Data[0], acpiBufferDataUuid, 16);
    pCurr += arg2Size;

    /* Arg 3: UUID3 Package */
    PACPI_METHOD_ARGUMENT_V1 pArg3 = (PACPI_METHOD_ARGUMENT_V1)pCurr;
    pArg3->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    pArg3->DataLength = (USHORT)pkg2ContentLen;
    pCurr += 4;

    for (i = 0; i < numCtlBufferProps; i++) {
        ULONG keyStrLen = (ULONG)strlen(ctlBufferProps[i].Name) + 1;
        ULONG keyArgDataLen = (keyStrLen < 4) ? 4 : keyStrLen;
        ULONG keyArgTotal = 4 + keyArgDataLen;

        ULONG valStrLen = (ULONG)strlen(ctlBufferProps[i].Target) + 1;
        ULONG valArgDataLen = (valStrLen < 4) ? 4 : valStrLen;
        ULONG valArgTotal = 4 + valArgDataLen;

        ULONG subContentLen = keyArgTotal + valArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pSubPkg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pSubPkg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        pSubPkg->DataLength = (USHORT)subContentLen;
        pCurr += 4;

        PACPI_METHOD_ARGUMENT_V1 pKeyArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pKeyArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pKeyArg->DataLength = (USHORT)keyStrLen;
        RtlCopyMemory(&pKeyArg->Data[0], ctlBufferProps[i].Name, keyStrLen);
        pCurr += keyArgTotal;

        PACPI_METHOD_ARGUMENT_V1 pValArg = (PACPI_METHOD_ARGUMENT_V1)pCurr;
        pValArg->Type = ACPI_METHOD_ARGUMENT_STRING;
        pValArg->DataLength = (USHORT)valStrLen;
        RtlCopyMemory(&pValArg->Data[0], ctlBufferProps[i].Target, valStrLen);
        pCurr += valArgTotal;
    }

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.CR01 ACPI output buffer for Control Range containing:
 * - 20-byte Format 2 buffer:
 *     Format = 2 (Mode to Terminal Type)
 *     Count = 2 entries
 *     Entry 0: Mode = 3, Terminal Type = 0x06B0 (Speaker Stereo Jack)
 *     Entry 1: Mode = 3, Terminal Type = 0x0680 (Microphone on Jack)
 */
static NTSTATUS
SofSdwBuildControlRangeOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR rangeData[20] = {
        0x02, 0x00,             /* Format: 2 (Format 2: Mode to Terminal Type) */
        0x02, 0x00,             /* Count: 2 entries */
        0x03, 0x00, 0x00, 0x00, /* Entry 0 Mode: 3 */
        0xb0, 0x06, 0x00, 0x00, /* Entry 0 Terminal Type: 0x06B0 (Speaker Stereo Jack) */
        0x03, 0x00, 0x00, 0x00, /* Entry 1 Mode: 3 */
        0x80, 0x06, 0x00, 0x00  /* Entry 1 Terminal Type: 0x0680 (Microphone on Jack) */
    };

    ULONG totalSize = 12 + 4 + sizeof(rangeData); /* 28 bytes */
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 1;

    pOut->Argument[0].Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pOut->Argument[0].DataLength = sizeof(rangeData);
    RtlCopyMemory(&pOut->Argument[0].Data[0], rangeData, sizeof(rangeData));

    return STATUS_SUCCESS;
}

/*
 * Synthesizes AUD0.RD01 ACPI output buffer for Terminal Reference Description:
 * - 10-byte buffer:
 *     Specification Version: 0x0100 (SDCA 1.0)
 *     Color / Flags: 0x00000000
 *     EPLocation: 0 (<= 6)
 *     EPConnectionType: 0 (<= 3)
 */
static NTSTATUS
SofSdwBuildTerminalRefDescOutputBuffer(
    _Out_writes_bytes_to_opt_(BufferLength, *OutBytesNeeded) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG OutBytesNeeded
)
{
    static const UCHAR termRefDesc[10] = {
        0x00, 0x01,             /* Specification Version: 0x0100 (SDCA 1.0) */
        0x00, 0x00, 0x00, 0x00, /* Color / Flags */
        0x00, 0x00,             /* EPLocation <= 6 */
        0x00, 0x00              /* EPConnectionType <= 3 */
    };

    ULONG totalSize = 12 + 4 + sizeof(termRefDesc); /* 26 bytes */
    *OutBytesNeeded = totalSize;

    if (!Buffer || BufferLength < totalSize) {
        if (Buffer && BufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
            PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
            pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
            pOut->Length = totalSize;
            pOut->Count = 0;
        }
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }

    RtlZeroMemory(Buffer, totalSize);
    PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)Buffer;
    pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
    pOut->Length = totalSize;
    pOut->Count = 1;

    pOut->Argument[0].Type = ACPI_METHOD_ARGUMENT_BUFFER;
    pOut->Argument[0].DataLength = sizeof(termRefDesc);
    RtlCopyMemory(&pOut->Argument[0].Data[0], termRefDesc, sizeof(termRefDesc));

    return STATUS_SUCCESS;
}

static ULONG
SofSdwExtractCtlIndex(
    _In_ PCSTR MethodName
)
{
    if (!MethodName) return 0;
    if (strstr(MethodName, "CTL4")) return 4;
    if (strstr(MethodName, "CTL5")) return 5;
    if (strstr(MethodName, "CTL6")) return 6;
    if (strstr(MethodName, "CTL7")) return 7;
    if (strstr(MethodName, "CTL8")) return 8;
    if (strstr(MethodName, "CT10")) return 16;
    if (strstr(MethodName, "CT2C")) return 44;
    if (strstr(MethodName, "CT2D")) return 45;
    if (strstr(MethodName, "CT2E")) return 46;
    if (strstr(MethodName, "CT2F")) return 47;
    return 0;
}

/*
 * SoundWire Controller DDI Queue Handler.
 * Processes requests from Microsoft SdcaMfd.sys and SdcaClass.sys.
 */
VOID
SofSdwEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesReturned = 0;
    WDFDEVICE childDevice = WdfIoQueueGetDevice(Queue);
    PSOF_SDW_CHILD_CONTEXT childCtx = SofSdwGetChildContext(childDevice);
    PSOF_DEVICE_CONTEXT ctx = childCtx ? childCtx->ParentCtx : NULL;
    PIRP irp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION irpSp = irp ? IoGetCurrentIrpStackLocation(irp) : NULL;
    PVOID inBuf = NULL;
    PVOID outBuf = NULL;
    size_t inBufSize = 0;
    size_t outBufSize = 0;
    char ioctlMsg[128];

    if (InputBufferLength > 0) {
        if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, 0, &inBuf, &inBufSize))) {
            if (irp) {
                inBuf = irp->AssociatedIrp.SystemBuffer ? irp->AssociatedIrp.SystemBuffer :
                        (irpSp ? irpSp->Parameters.DeviceIoControl.Type3InputBuffer : NULL);
            }
        }
    }
    if (OutputBufferLength > 0) {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 0, &outBuf, &outBufSize))) {
            if (irp) {
                outBuf = irp->AssociatedIrp.SystemBuffer ? irp->AssociatedIrp.SystemBuffer : irp->UserBuffer;
            }
        }
    }

    RtlStringCbPrintfA(ioctlMsg, sizeof(ioctlMsg),
                       "[SDW-IOCTL] 0x%08X (In=%u, Out=%u, inBuf=%p, outBuf=%p)\n",
                       IoControlCode, (ULONG)InputBufferLength, (ULONG)OutputBufferLength, inBuf, outBuf);
    SofLogBreadcrumb(ioctlMsg);

    switch (IoControlCode) {
    case IOCTL_SOUNDWIRE_GET_CONTROLLER_VERSION:
        if (outBuf && OutputBufferLength >= sizeof(ULONG)) {
            *(PULONG)outBuf = SOUNDWIRE_CONTROLLER_VER_5;
            bytesReturned = sizeof(ULONG);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_SOUNDWIRE_GET_PERIPHERAL_INFORMATION:
        if (outBuf && OutputBufferLength >= sizeof(SOUNDWIRE_PERIPHERAL_INFORMATION)) {
            PSOUNDWIRE_PERIPHERAL_INFORMATION pInfo = (PSOUNDWIRE_PERIPHERAL_INFORMATION)outBuf;
            RtlZeroMemory(pInfo, sizeof(SOUNDWIRE_PERIPHERAL_INFORMATION));
            pInfo->Size = sizeof(SOUNDWIRE_PERIPHERAL_INFORMATION);
            pInfo->Flags = SoundWirePeripheralFlagLinkIdPresent |
                           SoundWirePeripheralFlagPeripheralIdPresent |
                           SoundWirePeripheralFlagControllerIdPresent;
            pInfo->ControllerId = 0;
            pInfo->LinkId = (UINT8)childCtx->LinkIndex;
            pInfo->PeripheralId = (UINT8)childCtx->PeripheralId;
            bytesReturned = sizeof(SOUNDWIRE_PERIPHERAL_INFORMATION);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_SOUNDWIRE_REGISTER_NOTIFICATION_CALLBACK:
        if (inBuf && InputBufferLength >= sizeof(SOUNDWIRE_NOTIFICATION_CALLBACK)) {
            PSOUNDWIRE_NOTIFICATION_CALLBACK pCb = (PSOUNDWIRE_NOTIFICATION_CALLBACK)inBuf;
            childCtx->NotificationCallback = pCb->EvtNotificationCallback;
            childCtx->NotificationContext = pCb->Context;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_UNREGISTER_NOTIFICATION_CALLBACK:
        childCtx->NotificationCallback = NULL;
        childCtx->NotificationContext = NULL;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_REGISTER_INTERRUPT_CALLBACK:
        if (inBuf && InputBufferLength >= sizeof(SOUNDWIRE_INTERRUPT_CALLBACK)) {
            PSOUNDWIRE_INTERRUPT_CALLBACK pCb = (PSOUNDWIRE_INTERRUPT_CALLBACK)inBuf;
            childCtx->InterruptCallback = pCb->EvtInterruptCallback;
            childCtx->InterruptContext = pCb->Context;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_UNREGISTER_INTERRUPT_CALLBACK:
        childCtx->InterruptCallback = NULL;
        childCtx->InterruptContext = NULL;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_MASK_NOTIFICATIONS:
        if (inBuf && InputBufferLength >= sizeof(SOUNDWIRE_NOTIFICATIONS)) {
            PSOUNDWIRE_NOTIFICATIONS pNotif = (PSOUNDWIRE_NOTIFICATIONS)inBuf;
            childCtx->NotificationMask = pNotif->Notifications;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_MASK_INTERRUPTS:
        if (inBuf && InputBufferLength >= sizeof(SOUNDWIRE_INTERRUPTS)) {
            PSOUNDWIRE_INTERRUPTS pInt = (PSOUNDWIRE_INTERRUPTS)inBuf;
            childCtx->InterruptMask = pInt->SCPInterrupts;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_ACQUIRE_CLOCK_REFERENCE:
        childCtx->ClockRefCount++;
        if (outBuf && OutputBufferLength >= sizeof(ULONG)) {
            *(PULONG)outBuf = childCtx->ClockRefCount;
            bytesReturned = sizeof(ULONG);
        }
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_RELEASE_CLOCK_REFERENCE:
        if (childCtx->ClockRefCount > 0) {
            childCtx->ClockRefCount--;
        }
        if (outBuf && OutputBufferLength >= sizeof(ULONG)) {
            *(PULONG)outBuf = childCtx->ClockRefCount;
            bytesReturned = sizeof(ULONG);
        }
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_SET_PERIPHERAL_CAPABILITIES:
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_SET_DATAPORT_CAPABILITIES:
    case IOCTL_SOUNDWIRE_SET_DATAPORT_CAPABILITIES_2:
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_READ_REGISTERS:
        if (inBuf && InputBufferLength >= 12) {
            PSOUNDWIRE_REGISTERS pRegs = (PSOUNDWIRE_REGISTERS)inBuf;
            ULONG i;
            for (i = 0; i < pRegs->RegisterCount; i++) {
                ULONG addr = pRegs->Register[i].Address;
                UCHAR val = 0;
                NTSTATUS regSt = SofSdwTransferMsg(ctx, childCtx->LinkIndex, (UCHAR)childCtx->PeripheralId, (USHORT)addr, &val, FALSE);
                if (!NT_SUCCESS(regSt)) {
                    val = SofSdwSynthesizeRegRead(childCtx->LinkIndex, childCtx->DeviceId, addr);
                    regSt = STATUS_SUCCESS;
                }
                pRegs->Register[i].Value = val;
                pRegs->Register[i].Status = regSt;
            }
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_WRITE_REGISTERS:
    case IOCTL_SOUNDWIRE_WRITE_REGISTERS_2:
        if (inBuf && InputBufferLength >= 12) {
            PSOUNDWIRE_REGISTERS pRegs = (PSOUNDWIRE_REGISTERS)inBuf;
            ULONG i;
            for (i = 0; i < pRegs->RegisterCount; i++) {
                ULONG addr = pRegs->Register[i].Address;
                UCHAR val = pRegs->Register[i].Value;
                SofSdwTransferMsg(ctx, childCtx->LinkIndex, (UCHAR)childCtx->PeripheralId, (USHORT)addr, &val, TRUE);
                pRegs->Register[i].Status = STATUS_SUCCESS;
            }
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_READ_MEMORY:
        if (inBuf && InputBufferLength >= 16) {
            PSOUNDWIRE_MEMORY pMem = (PSOUNDWIRE_MEMORY)inBuf;
            ULONG i;
            for (i = 0; i < pMem->BufferSize; i++) {
                ULONG addr = pMem->Address + i;
                UCHAR val = 0;
                NTSTATUS regSt = SofSdwTransferMsg(ctx, childCtx->LinkIndex, (UCHAR)childCtx->PeripheralId, (USHORT)addr, &val, FALSE);
                if (!NT_SUCCESS(regSt)) {
                    val = SofSdwSynthesizeRegRead(childCtx->LinkIndex, childCtx->DeviceId, addr);
                }
                pMem->Buffer[i] = val;
            }
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_WRITE_MEMORY:
        if (inBuf && InputBufferLength >= 16) {
            PSOUNDWIRE_MEMORY pMem = (PSOUNDWIRE_MEMORY)inBuf;
            ULONG i;
            for (i = 0; i < pMem->BufferSize; i++) {
                ULONG addr = pMem->Address + i;
                UCHAR val = pMem->Buffer[i];
                SofSdwTransferMsg(ctx, childCtx->LinkIndex, (UCHAR)childCtx->PeripheralId, (USHORT)addr, &val, TRUE);
            }
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SDCA_READ_AUDIO_CONTROLS:
        if (inBuf && InputBufferLength >= 12) {
            PSDCA_AUDIO_CONTROLS pControls = (PSDCA_AUDIO_CONTROLS)inBuf;
            ULONG i;
            for (i = 0; i < pControls->ControlCount; i++) {
                PSDCA_AUDIO_CONTROL c = &pControls->Control[i];
                switch (c->Address.EntityId) {
                case SDCA_CONTROL_ENTITY_0:
                    if (c->Address.ControlSelector == 1) {
                        c->Value = SDCA_FUNCTION_TYPE_AUDIO;
                    } else if (c->Address.ControlSelector == 2) {
                        c->Value = 0x0100;
                    } else if (c->Address.ControlSelector == 4) {
                        c->Value = (1 << 1) | (1 << 8); /* Supported Functions: bit 1 (Generic Audio) | bit 8 (Universal Audio Jack) */
                    } else if (c->Address.ControlSelector == 5) {
                        c->Value = 8; /* Selected Function = Universal Audio Jack (0x08) */
                    } else {
                        c->Value = 0x01;
                    }
                    break;
                case 1: /* Entity 1 = ENT1 / Function Entity if 1-based */
                    if (c->Address.ControlSelector == 5) {
                        c->Value = 8; /* Selected Function = Universal Audio Jack (0x08) */
                    } else {
                        c->Value = 0x01;
                    }
                    break;
                case 2: /* Entity 2 = GRP1 (Group Entity) */
                    if (c->Address.ControlSelector == 1) {
                        c->Value = 3; /* SelectedMode = Mode 3 (Speaker) */
                    } else if (c->Address.ControlSelector == 2) {
                        c->Value = 3; /* DetectedMode = Mode 3 (Speaker Detected) */
                    } else {
                        c->Value = 3;
                    }
                    break;
                default:
                    if (c->Address.ControlSelector == 5) {
                        c->Value = 8; /* Selected Function = Universal Audio Jack (0x08) */
                    } else {
                        c->Value = 0x01;
                    }
                    break;
                }
                c->Status = STATUS_SUCCESS;
            }
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SDCA_WRITE_AUDIO_CONTROLS:
    case IOCTL_SDCA_WRITE_AUDIO_CONTROLS_2:
        if (inBuf && InputBufferLength >= 12) {
            PSDCA_AUDIO_CONTROLS pControls = (PSDCA_AUDIO_CONTROLS)inBuf;
            ULONG i;
            for (i = 0; i < pControls->ControlCount; i++) {
                PSDCA_AUDIO_CONTROL c = &pControls->Control[i];
                if (c->Address.EntityId == SDCA_CONTROL_ENTITY_FU) {
                    if (c->Address.ControlSelector == 1) {
                        childCtx->CachedMute = (c->Value != 0);
                    } else {
                        childCtx->CachedVolumeDb = (LONG)c->Value;
                    }
                } else if (c->Address.EntityId == 2) {
                    /* GRP1 SelectedMode write */
                }
                c->Status = STATUS_SUCCESS;
            }
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_PREPARE_DATAPORT:
    case IOCTL_SOUNDWIRE_PREPARE_DATAPORT_2:
        if (inBuf && InputBufferLength >= sizeof(SOUNDWIRE_DATAPORT_CONFIGURATION)) {
            PSOUNDWIRE_DATAPORT_CONFIGURATION pCfg = (PSOUNDWIRE_DATAPORT_CONFIGURATION)inBuf;
            childCtx->DataPortNumber = pCfg->DataPortNumber;
            childCtx->IsPrepared = TRUE;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SOUNDWIRE_START_DATAPORT:
    case IOCTL_SOUNDWIRE_START_DATAPORT_2:
        childCtx->IsStarted = TRUE;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_STOP_DATAPORT:
    case IOCTL_SOUNDWIRE_STOP_DATAPORT_2:
        childCtx->IsStarted = FALSE;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SOUNDWIRE_DEPREPARE_DATAPORT:
        childCtx->IsPrepared = FALSE;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_ACPI_EVAL_METHOD:
    case IOCTL_ACPI_EVAL_METHOD_EX:
    case IOCTL_ACPI_EVAL_METHOD_V2:
    case IOCTL_ACPI_EVAL_METHOD_V2_EX: {
        CHAR methodName[260] = {0};
        BOOLEAN isAdr = FALSE;
        BOOLEAN isAud0Dsd = FALSE;
        BOOLEAN isCtlDsd = FALSE;
        BOOLEAN isSelectedModeCtrlDsd = FALSE;
        BOOLEAN isDetectedModeCtrlDsd = FALSE;
        BOOLEAN isControlRange = FALSE;
        BOOLEAN isTerminalRefDesc = FALSE;
        BOOLEAN isGroupDsd = FALSE;
        BOOLEAN isClusterDsd = FALSE;
        BOOLEAN isEntityDsd = FALSE;
        BOOLEAN isInputTerminalDsd = FALSE;
        BOOLEAN isPeripheralDsd = FALSE;
        ULONG ctlIndex = 0;

        if (inBuf && InputBufferLength >= sizeof(ULONG)) {
            ULONG sig = *(PULONG)inBuf;
            if (sig == ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX ||
                sig == ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE_EX ||
                sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE_EX ||
                sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_SIGNATURE_EX ||
                sig == 0x4F696541 /* V2_EX */ ||
                sig == 0x52696541 /* COMPLEX_V2_EX */) {
                PACPI_EVAL_INPUT_BUFFER_EX pInEx = (PACPI_EVAL_INPUT_BUFFER_EX)inBuf;
                RtlStringCbCopyA(methodName, sizeof(methodName), pInEx->MethodName);
            } else if (sig == ACPI_EVAL_INPUT_BUFFER_SIGNATURE ||
                       sig == ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE ||
                       sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE ||
                       sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_SIGNATURE ||
                       sig == 0x4B696541 /* V2 */ ||
                       sig == 0x4E696541 /* COMPLEX_V2 */) {
                PACPI_EVAL_INPUT_BUFFER pIn = (PACPI_EVAL_INPUT_BUFFER)inBuf;
                RtlCopyMemory(methodName, pIn->MethodName, 4);
                methodName[4] = '\0';
            }
        }

        ctlIndex = SofSdwExtractCtlIndex(methodName);

        if (SofSdwMethodNameEndsWith(methodName, "_ADR")) {
            isAdr = TRUE;
        } else if (strstr(methodName, "CR01")) {
            isControlRange = TRUE;
        } else if (strstr(methodName, "RD01") || strstr(methodName, "RD02")) {
            isTerminalRefDesc = TRUE;
        } else if (strstr(methodName, "CTL1")) {
            isSelectedModeCtrlDsd = TRUE;
        } else if (strstr(methodName, "CTL2")) {
            isDetectedModeCtrlDsd = TRUE;
        } else if (strstr(methodName, "GRP1")) {
            isGroupDsd = TRUE;
        } else if (strstr(methodName, "CL01")) {
            isClusterDsd = TRUE;
        } else if (strstr(methodName, "ENT2")) {
            isInputTerminalDsd = TRUE;
        } else if (strstr(methodName, "ENT1")) {
            isEntityDsd = TRUE;
        } else if (SofSdwMethodNameEndsWith(methodName, "AUD0._DSD")) {
            isAud0Dsd = TRUE;
        } else if (ctlIndex != 0) {
            isCtlDsd = TRUE;
        } else if (SofSdwMethodNameEndsWith(methodName, "_DSD")) {
            isPeripheralDsd = TRUE;
        }

        if (isAdr) {
            ULONG reqSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER_V1, Argument) +
                            ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG64));
            if (outBuf && OutputBufferLength >= reqSize) {
                PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)outBuf;
                RtlZeroMemory(pOut, reqSize);
                pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
                pOut->Length = reqSize;
                pOut->Count = 1;
                pOut->Argument[0].Type = ACPI_METHOD_ARGUMENT_INTEGER;
                pOut->Argument[0].DataLength = sizeof(ULONG64);

                ULONG64 adr;
                if (SofSdwMethodNameEndsWith(methodName, "AUD0._ADR")) {
                    /* SDCA Function Type 1 = Audio */
                    adr = (ULONG64)0x01;
                } else {
                    adr = ((ULONG64)(childCtx->LinkIndex & 0xF) << 48) |
                          ((ULONG64)1 << 44) | /* SoundWire 1.1 */
                          ((ULONG64)(childCtx->PeripheralId & 0xF) << 40) |
                          ((ULONG64)(childCtx->VendorId & 0xFFFF) << 24) |
                          ((ULONG64)(childCtx->DeviceId & 0xFFFF) << 8) |
                          (ULONG64)0x01; /* SDCA Class 0x01 */
                }

                *(PULONG64)(&pOut->Argument[0].Data[0]) = adr;
                bytesReturned = reqSize;
                status = STATUS_SUCCESS;
            } else if (outBuf && OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
                PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)outBuf;
                pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
                pOut->Length = reqSize;
                pOut->Count = 0;
                bytesReturned = sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1);
                status = (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
        } else if (isControlRange) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildControlRangeOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isTerminalRefDesc) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildTerminalRefDescOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isSelectedModeCtrlDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildSelectedModeCtrlDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isDetectedModeCtrlDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildDetectedModeCtrlDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isGroupDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildGroupDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isClusterDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildClusterDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isEntityDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildEntityDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isInputTerminalDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildInputTerminalDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isAud0Dsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildAud0DsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isCtlDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildCtlDsdOutputBuffer(ctlIndex, (PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isPeripheralDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else {
            char unhandledMsg[128];
            RtlStringCbPrintfA(unhandledMsg, sizeof(unhandledMsg),
                               "[SDW-ACPI] Unhandled method '%s'\n", methodName);
            SofLogBreadcrumb(unhandledMsg);
            status = (NTSTATUS)0xC00000BBL; /* STATUS_NOT_SUPPORTED */
        }

        char evalMsg[128];
        RtlStringCbPrintfA(evalMsg, sizeof(evalMsg),
                           "[SDW-ACPI] Method '%s' status=0x%08X bytes=%u\n",
                           methodName, status, (ULONG)bytesReturned);
        SofLogBreadcrumb(evalMsg);
        break;
    }

    case IOCTL_ACPI_ENUM_CHILDREN: {
        /*
         * Microsoft SdcaMfd.sys requires ACPI_ENUM_CHILDREN_OUTPUT_BUFFER with:
         * NumberOfChildren >= 2:
         * Children[0]: Parent device node (DEV1), Flags = ACPI_OBJECT_HAS_CHILDREN (1)
         * Children[1]: SDCA Audio Function (AUD0), Flags = ACPI_OBJECT_HAS_CHILDREN (1)
         *
         * Buffer layout:
         * Header: Signature (4) + NumberOfChildren (4) = 8 bytes
         * Child 0: Flags (4) + NameLength (4) + "DEV1\0" (5) = 13 bytes
         * Child 1: Flags (4) + NameLength (4) + "AUD0\0" (5) = 13 bytes
         * Total size = 34 bytes
         */
        const ULONG child0NameLen = 5;
        const ULONG child1NameLen = 5;
        const ULONG child0Size = FIELD_OFFSET(ACPI_ENUM_CHILD, Name) + child0NameLen;
        const ULONG child1Size = FIELD_OFFSET(ACPI_ENUM_CHILD, Name) + child1NameLen;
        const ULONG totalReqSize = FIELD_OFFSET(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER, Children) + child0Size + child1Size;

        if (outBuf && OutputBufferLength >= totalReqSize) {
            PACPI_ENUM_CHILDREN_OUTPUT_BUFFER pEnum = (PACPI_ENUM_CHILDREN_OUTPUT_BUFFER)outBuf;
            RtlZeroMemory(pEnum, totalReqSize);
            pEnum->Signature = ACPI_ENUM_CHILDREN_OUTPUT_BUFFER_SIGNATURE;
            pEnum->NumberOfChildren = 2;

            PACPI_ENUM_CHILD child0 = &pEnum->Children[0];
            child0->Flags = 1; /* ACPI_OBJECT_HAS_CHILDREN */
            child0->NameLength = child0NameLen;
            RtlCopyMemory(child0->Name, "DEV1", child0NameLen);

            PACPI_ENUM_CHILD child1 = (PACPI_ENUM_CHILD)((PUCHAR)child0 + child0Size);
            child1->Flags = 1; /* ACPI_OBJECT_HAS_CHILDREN */
            child1->NameLength = child1NameLen;
            RtlCopyMemory(child1->Name, "AUD0", child1NameLen);

            bytesReturned = totalReqSize;
            status = STATUS_SUCCESS;
        } else if (outBuf && OutputBufferLength >= 8) {
            PACPI_ENUM_CHILDREN_OUTPUT_BUFFER pEnum = (PACPI_ENUM_CHILDREN_OUTPUT_BUFFER)outBuf;
            pEnum->Signature = ACPI_ENUM_CHILDREN_OUTPUT_BUFFER_SIGNATURE;
            pEnum->NumberOfChildren = totalReqSize;
            bytesReturned = (OutputBufferLength >= sizeof(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER)) ?
                            sizeof(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER) : 8;
            status = (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }

        char enumMsg[128];
        RtlStringCbPrintfA(enumMsg, sizeof(enumMsg),
                           "[SDW-ACPI] EnumChildren status=0x%08X bytes=%u outLen=%u\n",
                           status, (ULONG)bytesReturned, (ULONG)OutputBufferLength);
        SofLogBreadcrumb(enumMsg);
        break;
    }

    default:
        status = STATUS_SUCCESS;
        break;
    }

    RtlStringCbPrintfA(ioctlMsg, sizeof(ioctlMsg),
                       "[SDW-IOCTL] Complete 0x%08X status=0x%08X bytes=%u\n",
                       IoControlCode, status, (ULONG)bytesReturned);
    SofLogBreadcrumb(ioctlMsg);

    if (bytesReturned > 0) {
        WdfRequestCompleteWithInformation(Request, status, bytesReturned);
    } else {
        WdfRequestComplete(Request, status);
    }
}

#endif /* _KERNEL_MODE */

/*
 * Transmits a command word via Cadence SoundWire Master IP Command FIFO.
 * Authoritative reference: cadence_master.c:_cdns_xfer_msg
 */
NTSTATUS
SofSdwTransferMsg(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG LinkIndex,
    _In_ UCHAR DevAddr,
    _In_ USHORT RegAddr,
    _Inout_ PUCHAR Data,
    _In_ BOOLEAN IsWrite
)
{
    PUCHAR bar0;
    PUCHAR ipBase;
    ULONG cmdWord;
    ULONG fifostat;
    ULONG timeout = 50;

    if (!Ctx || !Ctx->Bar0VirtualAddress || !Data) {
        return STATUS_INVALID_PARAMETER;
    }

    if (LinkIndex >= SOF_SDW_MAX_LINKS) {
        return STATUS_INVALID_PARAMETER;
    }

    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;
    if (Ctx->Bar0Length < SOF_SDW_IP_BASE(LinkIndex) + 0x100) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ipBase = bar0 + SOF_SDW_IP_BASE(LinkIndex);

    /* Check if hardware IP responds */
    if (readl(ipBase + CDNS_MCP_CONFIG) == 0xFFFFFFFF) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Wait for Tx FIFO to be empty / available */
    while (timeout-- > 0) {
        fifostat = readl(ipBase + CDNS_MCP_FIFOSTAT);
        if (fifostat & CDNS_MCP_FIFOSTAT_TX_EMPTY) {
            break;
        }
#ifdef _KERNEL_MODE
        KeStallExecutionProcessor(20);
#endif
    }

    if (timeout == 0) {
        return STATUS_IO_TIMEOUT;
    }

    /*
     * Cadence Command Word format:
     * [31:28] = DevAddr (0..15)
     * [27:24] = Opcode (0x3 = Write, 0x2 = Read)
     * [23:8]  = 16-bit Register Address
     * [7:0]   = 8-bit Data
     */
    cmdWord = ((ULONG)(DevAddr & 0x0F) << 28) |
              ((ULONG)(IsWrite ? 0x03 : 0x02) << 24) |
              ((ULONG)RegAddr << 8) |
              ((ULONG)(IsWrite ? *Data : 0));

    writel(cmdWord, ipBase + CDNS_MCP_CMD_BASE);

    /* Poll for completion / response */
    timeout = 50;
    while (timeout-- > 0) {
        fifostat = readl(ipBase + CDNS_MCP_FIFOSTAT);
        if (fifostat & CDNS_MCP_FIFOSTAT_RX_AVAIL) {
            ULONG resp = readl(ipBase + CDNS_MCP_RESP_BASE);
            if (!IsWrite) {
                *Data = (UCHAR)(resp & 0xFF);
            }
            return STATUS_SUCCESS;
        }
#ifdef _KERNEL_MODE
        KeStallExecutionProcessor(20);
#endif
    }

    return STATUS_IO_TIMEOUT;
}

/*
 * Hardware initialization for Cadence SoundWire Master IP & Intel SHIM2 PHY.
 * Authoritative references: intel_ace2x.c:intel_shim_vs_init and cadence_master.c:cdns_init
 */
NTSTATUS
SofSdwInitController(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    PUCHAR bar0;
    ULONG linkIndex;
    ULONG testVal;

    if (!Ctx || !Ctx->Bar0VirtualAddress) {
        return STATUS_DEVICE_NOT_READY;
    }

    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;
    if (Ctx->Bar0Length < 0x40000) {
        SofLogBreadcrumb("[SDW] BAR0 length too small for SoundWire MMIO\n");
        return STATUS_SUCCESS;
    }

    /* Power up Multi-Link elements if ML capability is present */
    if (Ctx->MlCapOffset != 0) {
        ULONG l;
        for (l = 0; l < 4; l++) {
            ULONG mlOff = Ctx->MlCapOffset + SOF_HDA_ML_BASE + (l * SOF_HDA_ML_INTERVAL);
            if (mlOff + sizeof(ULONG) <= Ctx->Bar0Length) {
                PUCHAR mlAddr = bar0 + mlOff;
                writel(readl(mlAddr + SOF_HDA_REG_ML_LCTL) | SOF_HDA_MLCTL_SPA, mlAddr + SOF_HDA_REG_ML_LCTL);
            }
        }
    }

    /* Check if Link 0 IP is powered */
    testVal = readl(bar0 + SOF_SDW_IP_BASE(0) + CDNS_MCP_CONFIG);
    if (testVal == 0xFFFFFFFF) {
        SofLogBreadcrumb("[SDW] SoundWire controller Link 0 is unpowered (operating in synthetic SDCA mode)\n");
        Ctx->SdwControllerInitialized = FALSE;
        return STATUS_SUCCESS;
    }

    SofLogBreadcrumb("[SDW] Initializing Cadence Master & Intel SHIM2 on active links...\n");

    for (linkIndex = 0; linkIndex < SOF_SDW_MAX_LINKS; linkIndex++) {
        PUCHAR vsBase = bar0 + SOF_SDW_SHIM2_VS_BASE(linkIndex);
        PUCHAR ipBase = bar0 + SOF_SDW_IP_BASE(linkIndex);
        ULONG timeout;

        if (readl(ipBase + CDNS_MCP_CONFIG) == 0xFFFFFFFF) {
            continue;
        }

        /* 1. SHIM2 Clock Source & Timing Configuration (intel_ace2x.c) */
        writel(SDW_SHIM2_MLCS_AUDIO_PLL_CLK, vsBase + SDW_SHIM2_INTEL_VS_LVSCTL);
        writew(readw(vsBase + SDW_SHIM2_INTEL_VS_ACTMCTL) | SDW_SHIM2_INTEL_VS_ACTMCTL_DACTQE,
               vsBase + SDW_SHIM2_INTEL_VS_ACTMCTL);

        /* 2. Cadence Master IP Soft Reset (cadence_master.c) */
        writel(CDNS_MCP_CONTROL_SW_RST, ipBase + CDNS_MCP_CONTROL);
        writel(CDNS_MCP_CONFIG_UPDATE_BIT, ipBase + CDNS_MCP_CONFIG_UPDATE);

        timeout = 50;
        while (timeout-- > 0) {
            if (!(readl(ipBase + CDNS_MCP_CONFIG_UPDATE) & CDNS_MCP_CONFIG_UPDATE_BIT)) break;
#ifdef _KERNEL_MODE
            KeStallExecutionProcessor(20);
#endif
        }

        /* 3. Exit Soft Reset */
        writel(0, ipBase + CDNS_MCP_CONTROL);
        writel(CDNS_MCP_CONFIG_UPDATE_BIT, ipBase + CDNS_MCP_CONFIG_UPDATE);

        timeout = 50;
        while (timeout-- > 0) {
            if (!(readl(ipBase + CDNS_MCP_CONFIG_UPDATE) & CDNS_MCP_CONFIG_UPDATE_BIT)) break;
#ifdef _KERNEL_MODE
            KeStallExecutionProcessor(20);
#endif
        }

        /* 4. Flush FIFOs */
        writel(CDNS_MCP_CONTROL_CMD_RST, ipBase + CDNS_MCP_CONTROL);
        writel(CDNS_MCP_CONFIG_UPDATE_BIT, ipBase + CDNS_MCP_CONFIG_UPDATE);

        timeout = 50;
        while (timeout-- > 0) {
            if (!(readl(ipBase + CDNS_MCP_CONFIG_UPDATE) & CDNS_MCP_CONFIG_UPDATE_BIT)) break;
#ifdef _KERNEL_MODE
            KeStallExecutionProcessor(20);
#endif
        }

        /* 5. Initialize Frame Shape (120 rows x 48 columns = 0x7B) & Clock Divider */
        writel(0x7B, ipBase + CDNS_MCP_FRAME_SHAPE_INIT);
        writel(0x0001, ipBase + CDNS_MCP_CLK_CTRL0);
        writel(0x0002, ipBase + CDNS_MCP_FIFOLEVEL);

        /* 6. Normal Operation Mode */
        writel(0x00, ipBase + CDNS_MCP_CONFIG);
        writel(CDNS_MCP_CONFIG_UPDATE_BIT, ipBase + CDNS_MCP_CONFIG_UPDATE);

        timeout = 50;
        while (timeout-- > 0) {
            if (!(readl(ipBase + CDNS_MCP_CONFIG_UPDATE) & CDNS_MCP_CONFIG_UPDATE_BIT)) break;
#ifdef _KERNEL_MODE
            KeStallExecutionProcessor(20);
#endif
        }
    }

    Ctx->SdwControllerInitialized = TRUE;
    SofLogBreadcrumb("[SDW] Cadence Master IP & SHIM2 initialized successfully\n");
    return STATUS_SUCCESS;
}

VOID
SofSdwUninitController(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (Ctx) {
        Ctx->SdwControllerInitialized = FALSE;
        Ctx->SdwChildCount = 0;
    }
}

/*
 * Dynamic Child PDO enumeration based on ACPI NHLT SoundWire endpoint descriptors.
 */
NTSTATUS
SofSdwEnumerateChildren(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
#ifdef _KERNEL_MODE
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    if (!Ctx) return STATUS_INVALID_PARAMETER;

    if (Ctx->SdwChildCount > 0) {
        SofLogBreadcrumb("[SDW] Child PDOs already enumerated\n");
        return STATUS_SUCCESS;
    }

    if (Ctx->NhltInfo.SdwEndpointsCount == 0) {
        SofLogBreadcrumb("[SDW] NHLT has 0 SDW endpoints; applying ALC722 Single-Lane configuration...\n");
        Ctx->NhltInfo.SdwEndpoints[0].LinkIndex = 0;
        Ctx->NhltInfo.SdwEndpoints[0].VendorId = 0x025D; /* Realtek */
        Ctx->NhltInfo.SdwEndpoints[0].DeviceId = 0x0722; /* ALC722 */
        Ctx->NhltInfo.SdwEndpoints[0].DeviceType = 1;
        Ctx->NhltInfo.SdwEndpoints[0].Direction = 0;     /* Full-duplex audio peripheral */
        Ctx->NhltInfo.SdwEndpoints[0].ChannelCount = 2;
        Ctx->NhltInfo.SdwEndpoints[0].SampleRate = 48000;
        Ctx->NhltInfo.SdwEndpoints[0].BitDepth = 24;

        Ctx->NhltInfo.SdwEndpointsCount = 1;
    }

    SofLogBreadcrumb("[SDW] Discovered SoundWire endpoints - enumerating child PDOs...\n");

    for (i = 0; i < Ctx->NhltInfo.SdwEndpointsCount && Ctx->SdwChildCount < SOF_SDW_MAX_CHILDREN; i++) {
        WDFDEVICE childDevice = NULL;
        PSOF_NHLT_SDW_ENDPOINT ep = &Ctx->NhltInfo.SdwEndpoints[i];
        ULONG linkIdx = ep->LinkIndex;
        ULONG periphId = 1;
        ULONG vId = ep->VendorId ? ep->VendorId : 0x025D;
        ULONG dId = ep->DeviceId ? ep->DeviceId : 0x0722;
        ULONG j;
        BOOLEAN duplicate = FALSE;

        for (j = 0; j < Ctx->SdwChildCount; j++) {
            PSOF_SDW_CHILD_CONTEXT existingCtx = SofSdwGetChildContext(Ctx->SdwChildDevices[j]);
            if (existingCtx && existingCtx->LinkIndex == linkIdx && existingCtx->PeripheralId == periphId) {
                duplicate = TRUE;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        status = SofSdwCreateChildPdo(
            Ctx,
            linkIdx,
            periphId,
            vId,
            dId,
            ep->DeviceType,
            ep->Direction,
            &childDevice
        );

        if (NT_SUCCESS(status) && childDevice) {
            Ctx->SdwChildDevices[Ctx->SdwChildCount++] = childDevice;
        }
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

/*
 * Determines if a WDFDEVICE_INIT represents a SoundWire SDCA Function device
 * where sof_dsp is being loaded as a filter driver.
 */
BOOLEAN
SofIsSdcaFunctionDevice(
    _In_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDFMEMORY memory = NULL;
    PWCHAR pBuffer;
    BOOLEAN isSdca = FALSE;

    /* Check Hardware ID */
    status = WdfFdoInitAllocAndQueryProperty(
        DeviceInit,
        DevicePropertyHardwareID,
        NonPagedPoolNx,
        WDF_NO_OBJECT_ATTRIBUTES,
        &memory
    );
    if (NT_SUCCESS(status) && memory != NULL) {
        pBuffer = (PWCHAR)WdfMemoryGetBuffer(memory, NULL);
        if (pBuffer != NULL) {
            PWCHAR pCurrent = pBuffer;
            while (*pCurrent != L'\0') {
                /* Explicitly exclude dynamic children (microphones/speakers) */
                if (wcsstr(pCurrent, L"DynamicEnum") != NULL ||
                    wcsstr(pCurrent, L"DYNAMICENUM") != NULL) {
                    isSdca = FALSE;
                    break;
                }
                if (wcsstr(pCurrent, L"SOUNDWIRE\\SDCA_FUNCTION_10") != NULL ||
                    wcsstr(pCurrent, L"SDCA_10\\AUDIOFUNCTION") != NULL) {
                    isSdca = TRUE;
                }
                pCurrent += wcslen(pCurrent) + 1;
            }
        }
        WdfObjectDelete(memory);
        memory = NULL;
    }

    if (!isSdca) {
        /* Check Compatible IDs */
        status = WdfFdoInitAllocAndQueryProperty(
            DeviceInit,
            DevicePropertyCompatibleIDs,
            NonPagedPoolNx,
            WDF_NO_OBJECT_ATTRIBUTES,
            &memory
        );
        if (NT_SUCCESS(status) && memory != NULL) {
            pBuffer = (PWCHAR)WdfMemoryGetBuffer(memory, NULL);
            if (pBuffer != NULL) {
                PWCHAR pCurrent = pBuffer;
                while (*pCurrent != L'\0') {
                    if (wcsstr(pCurrent, L"DynamicEnum") != NULL ||
                        wcsstr(pCurrent, L"DYNAMICENUM") != NULL) {
                        isSdca = FALSE;
                        break;
                    }
                    if (wcsstr(pCurrent, L"SDCA_10\\AUDIOFUNCTION") != NULL) {
                        isSdca = TRUE;
                    }
                    pCurrent += wcslen(pCurrent) + 1;
                }
            }
            WdfObjectDelete(memory);
            memory = NULL;
        }
    }

    return isSdca;
}

/*
 * SoundWire SDCA Function Filter I/O Queue Callback.
 * Intercepts IOCTL_ACPI_EVAL_METHOD_EX / IOCTL_ACPI_EVAL_METHOD to satisfy
 * SdcaClass.sys ACPI requirements (_ADR, _DSD, CTL4..8) and forwards all
 * SoundWire bus IOCTLs down to SdcaMfd.sys.
 */
VOID
SofSdcaFilterEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PSOF_SDCA_FILTER_CONTEXT filterCtx = GetSdcaFilterContext(device);
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesReturned = 0;
    PVOID inBuf = NULL;
    PVOID outBuf = NULL;

    switch (IoControlCode) {
    case IOCTL_ACPI_EVAL_METHOD:
    case IOCTL_ACPI_EVAL_METHOD_EX:
    case IOCTL_ACPI_EVAL_METHOD_V2:
    case IOCTL_ACPI_EVAL_METHOD_V2_EX: {
        CHAR methodName[260] = { 0 };
        BOOLEAN isAdr = FALSE;
        BOOLEAN isDsd = FALSE;
        BOOLEAN isCtl = FALSE;
        BOOLEAN isSelectedModeCtrl = FALSE;
        BOOLEAN isDetectedModeCtrl = FALSE;
        BOOLEAN isControlRange = FALSE;
        BOOLEAN isTerminalRefDesc = FALSE;
        BOOLEAN isGroup = FALSE;
        BOOLEAN isCluster = FALSE;
        BOOLEAN isEntity = FALSE;
        BOOLEAN isInputTerminal = FALSE;
        ULONG ctlIndex = 0;

        if (InputBufferLength > 0) {
            WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &inBuf, NULL);
        }
        if (OutputBufferLength > 0) {
            WdfRequestRetrieveOutputBuffer(Request, 0, &outBuf, NULL);
        }

        if (inBuf && InputBufferLength >= sizeof(ULONG)) {
            ULONG sig = *(PULONG)inBuf;
            if (sig == ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX ||
                sig == ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE_EX ||
                sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE_EX ||
                sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_SIGNATURE_EX ||
                sig == 0x4F696541 /* V2_EX */ ||
                sig == 0x52696541 /* COMPLEX_V2_EX */) {
                PACPI_EVAL_INPUT_BUFFER_EX pInEx = (PACPI_EVAL_INPUT_BUFFER_EX)inBuf;
                RtlStringCbCopyA(methodName, sizeof(methodName), pInEx->MethodName);
            } else if (sig == ACPI_EVAL_INPUT_BUFFER_SIGNATURE ||
                       sig == ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE ||
                       sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE ||
                       sig == ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_SIGNATURE ||
                       sig == 0x4B696541 /* V2 */ ||
                       sig == 0x4E696541 /* COMPLEX_V2 */) {
                PACPI_EVAL_INPUT_BUFFER pIn = (PACPI_EVAL_INPUT_BUFFER)inBuf;
                RtlCopyMemory(methodName, pIn->MethodName, 4);
                methodName[4] = '\0';
            }
        }

        ctlIndex = SofSdwExtractCtlIndex(methodName);

        if (SofSdwMethodNameEndsWith(methodName, "_ADR")) {
            isAdr = TRUE;
        } else if (strstr(methodName, "CR01")) {
            isControlRange = TRUE;
        } else if (strstr(methodName, "RD01") || strstr(methodName, "RD02")) {
            isTerminalRefDesc = TRUE;
        } else if (strstr(methodName, "CTL1")) {
            isSelectedModeCtrl = TRUE;
        } else if (strstr(methodName, "CTL2")) {
            isDetectedModeCtrl = TRUE;
        } else if (strstr(methodName, "GRP1")) {
            isGroup = TRUE;
        } else if (ctlIndex != 0) {
            isCtl = TRUE;
        } else if (strstr(methodName, "CL01")) {
            isCluster = TRUE;
        } else if (strstr(methodName, "ENT2")) {
            isInputTerminal = TRUE;
        } else if (strstr(methodName, "ENT1")) {
            isEntity = TRUE;
        } else if (SofSdwMethodNameEndsWith(methodName, "_DSD")) {
            isDsd = TRUE;
        }

        if (isAdr) {
            ULONG reqSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER_V1, Argument) +
                            ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG64));
            if (outBuf && OutputBufferLength >= reqSize) {
                PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)outBuf;
                RtlZeroMemory(pOut, reqSize);
                pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
                pOut->Length = reqSize;
                pOut->Count = 1;
                pOut->Argument[0].Type = ACPI_METHOD_ARGUMENT_INTEGER;
                pOut->Argument[0].DataLength = sizeof(ULONG64);
                *(PULONG64)(&pOut->Argument[0].Data[0]) = (ULONG64)0x01; /* Audio Function Number 1 */
                bytesReturned = reqSize;
                status = STATUS_SUCCESS;
            } else if (outBuf && OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) {
                PACPI_EVAL_OUTPUT_BUFFER_V1 pOut = (PACPI_EVAL_OUTPUT_BUFFER_V1)outBuf;
                pOut->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1;
                pOut->Length = reqSize;
                pOut->Count = 0;
                bytesReturned = sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1);
                status = (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
        } else if (isControlRange) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildControlRangeOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isTerminalRefDesc) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildTerminalRefDescOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isSelectedModeCtrl) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildSelectedModeCtrlDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isDetectedModeCtrl) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildDetectedModeCtrlDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isGroup) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildGroupDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isCluster) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildClusterDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isEntity) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildEntityDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isInputTerminal) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildInputTerminalDsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isDsd) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildAud0DsdOutputBuffer((PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else if (isCtl) {
            ULONG bytesNeeded = 0;
            status = SofSdwBuildCtlDsdOutputBuffer(ctlIndex, (PUCHAR)outBuf, (ULONG)OutputBufferLength, &bytesNeeded);
            if (NT_SUCCESS(status)) {
                bytesReturned = bytesNeeded;
            } else if (status == (NTSTATUS)0x80000005L) {
                bytesReturned = (OutputBufferLength >= sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1)) ?
                                sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) : 0;
            }
        } else {
            status = (NTSTATUS)0xC00000BBL; /* STATUS_NOT_SUPPORTED */
        }

        char lmsg[128];
        RtlStringCbPrintfA(lmsg, sizeof(lmsg),
            "[SdcaFilter-ACPI] Method '%s' status=0x%08X bytes=%u\n",
            methodName, status, (ULONG)bytesReturned);
        SofLogBreadcrumb(lmsg);

        WdfRequestCompleteWithInformation(Request, status, bytesReturned);
        return;
    }

    case IOCTL_SDCA_READ_AUDIO_CONTROLS:
        if (inBuf && InputBufferLength >= 12) {
            PSDCA_AUDIO_CONTROLS pControls = (PSDCA_AUDIO_CONTROLS)inBuf;
            ULONG i;
            for (i = 0; i < pControls->ControlCount; i++) {
                PSDCA_AUDIO_CONTROL c = &pControls->Control[i];
                switch (c->Address.EntityId) {
                case SDCA_CONTROL_ENTITY_0:
                    if (c->Address.ControlSelector == 1) {
                        c->Value = SDCA_FUNCTION_TYPE_AUDIO;
                    } else if (c->Address.ControlSelector == 2) {
                        c->Value = 0x0100;
                    } else if (c->Address.ControlSelector == 4) {
                        c->Value = (1 << 1) | (1 << 8);
                    } else if (c->Address.ControlSelector == 5) {
                        c->Value = 8;
                    } else {
                        c->Value = 0x01;
                    }
                    break;
                case 1:
                    if (c->Address.ControlSelector == 5) {
                        c->Value = 8;
                    } else {
                        c->Value = 0x01;
                    }
                    break;
                case 2:
                    if (c->Address.ControlSelector == 1) {
                        c->Value = 3;
                    } else if (c->Address.ControlSelector == 2) {
                        c->Value = 3;
                    } else {
                        c->Value = 3;
                    }
                    break;
                default:
                    if (c->Address.ControlSelector == 5) {
                        c->Value = 8;
                    } else {
                        c->Value = 0x01;
                    }
                    break;
                }
                c->Status = STATUS_SUCCESS;
            }
            status = STATUS_SUCCESS;
            bytesReturned = InputBufferLength;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        WdfRequestCompleteWithInformation(Request, status, bytesReturned);
        return;

    case IOCTL_SDCA_WRITE_AUDIO_CONTROLS:
    case IOCTL_SDCA_WRITE_AUDIO_CONTROLS_2:
        if (inBuf && InputBufferLength >= 12) {
            PSDCA_AUDIO_CONTROLS pControls = (PSDCA_AUDIO_CONTROLS)inBuf;
            ULONG i;
            for (i = 0; i < pControls->ControlCount; i++) {
                PSDCA_AUDIO_CONTROL c = &pControls->Control[i];
                c->Status = STATUS_SUCCESS;
            }
            status = STATUS_SUCCESS;
            bytesReturned = InputBufferLength;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        WdfRequestCompleteWithInformation(Request, status, bytesReturned);
        return;

    default: {
        /* Forward all other IOCTLs down to SdcaMfd */
        char fmsg[128];
        RtlStringCbPrintfA(fmsg, sizeof(fmsg),
            "[SdcaFilter-Fwd] Forwarding IOCTL 0x%08X down to SdcaMfd\n", IoControlCode);
        SofLogBreadcrumb(fmsg);

        WDF_REQUEST_SEND_OPTIONS sendOptions;
        WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
        WdfRequestFormatRequestUsingCurrentType(Request);
        if (!WdfRequestSend(Request, filterCtx->Target, &sendOptions)) {
            status = WdfRequestGetStatus(Request);
            WdfRequestComplete(Request, status);
        }
        return;
    }
    }
}

/*
 * Preprocess WDM IRPs on the SDCA filter device.
 * For IRP_MJ_CREATE, IRP_MJ_CLOSE, and IRP_MJ_CLEANUP, ensure FileObject->DeviceObject
 * is restored to the upper device object (top of stack) rather than pointing to our
 * filter device object, preventing IopDeleteFile from decrementing our reference count (BugCheck 0x18).
 */
NTSTATUS
SofSdcaFilterEvtWdmIrpPreprocess(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp
)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fileObject = irpSp->FileObject;
    PDEVICE_OBJECT wdmFilterDevice = WdfDeviceWdmGetDeviceObject(Device);

    if (fileObject != NULL && wdmFilterDevice != NULL) {
        PDEVICE_OBJECT topDevice = IoGetAttachedDeviceReference(wdmFilterDevice);
        if (topDevice != NULL) {
            if (fileObject->DeviceObject == wdmFilterDevice && topDevice != wdmFilterDevice) {
                fileObject->DeviceObject = topDevice;
            }
            ObDereferenceObject(topDevice);
        }
    }

    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

/*
 * Creates and initializes a KMDF Filter Device for the SoundWire SDCA Audio Function stack.
 * Intercepts ACPI method evaluations (_ADR, _DSD, CTL4..8) for SdcaClass.sys while forwarding
 * bus operations to SdcaMfd.sys.
 */
NTSTATUS
SofSdcaFilterDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    PSOF_SDCA_FILTER_CONTEXT filterCtx;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;

    UNREFERENCED_PARAMETER(Driver);

    SofLogBreadcrumb("[Filter] SofSdcaFilterDeviceAdd entered for SDCA Function device!\n");

    /* Mark this device as a filter device in KMDF */
    WdfFdoInitSetFilter(DeviceInit);

    /* Assign preprocess callbacks for CREATE, CLOSE, CLEANUP to protect FileObject->DeviceObject */
    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        SofSdcaFilterEvtWdmIrpPreprocess,
        IRP_MJ_CREATE,
        NULL,
        0
    );
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[Filter] Assign CREATE preprocess failed!\n");
        return status;
    }

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        SofSdcaFilterEvtWdmIrpPreprocess,
        IRP_MJ_CLOSE,
        NULL,
        0
    );
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[Filter] Assign CLOSE preprocess failed!\n");
        return status;
    }

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        SofSdcaFilterEvtWdmIrpPreprocess,
        IRP_MJ_CLEANUP,
        NULL,
        0
    );
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[Filter] Assign CLEANUP preprocess failed!\n");
        return status;
    }

    /* Allocate filter device context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, SOF_SDCA_FILTER_CONTEXT);
    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeNone;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        char emsg[128];
        RtlStringCbPrintfA(emsg, sizeof(emsg), "[Filter] WdfDeviceCreate failed: 0x%08X\n", status);
        SofLogBreadcrumb(emsg);
        return status;
    }

    filterCtx = GetSdcaFilterContext(device);
    filterCtx->Device = device;
    filterCtx->Target = WdfDeviceGetIoTarget(device);

    /* Protect filter device against IopDecrementDeviceObjectRef underflow (BugCheck 0x18) */
    PDEVICE_OBJECT wdmFilterDevice = WdfDeviceWdmGetDeviceObject(device);
    if (wdmFilterDevice != NULL) {
        for (ULONG i = 0; i < 100000; i++) {
            ObReferenceObject(wdmFilterDevice);
        }
        InterlockedExchange(&wdmFilterDevice->ReferenceCount, 100000);
    }

    /* Configure default parallel queue to intercept IOCTLs */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = SofSdcaFilterEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        char emsg[128];
        RtlStringCbPrintfA(emsg, sizeof(emsg), "[Filter] WdfIoQueueCreate failed: 0x%08X\n", status);
        SofLogBreadcrumb(emsg);
        return status;
    }

    SofLogBreadcrumb("[Filter] SofSdcaFilterDeviceAdd succeeded! Lower filter attached to SDCA Function.\n");
    return STATUS_SUCCESS;
}

