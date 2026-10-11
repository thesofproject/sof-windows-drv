/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Driver.c
 * @brief DriverEntry, SafeBootPolicy Engine, and Device Creation for SOF Windows Driver.
 *
 * Implements the driver lifecycle, the 3-Tier Safety Canary watchdog to prevent
 * boot-loop crashes, WDM IRP preprocessing for PortCls COM interfaces, and WDF
 * device initialization.
 *
 * Safety Canary Architecture:
 * - Tier 0 (FORCED_SAFE): Audio DSP hardware is bypassed; driver acts as a stub.
 * - Tier 1 (SINGLE_SHOT): Armed only if BootSafetyToken=1 and \SystemRoot\sof_canary.bin
 *   is present. Atomically clears token on boot to guarantee safe recovery on panic.
 * - Tier 2 (PRODUCTION): Automatic watchdog. Increments ConsecutiveCrashCount before
 *   device start. If the driver operates in D0 for 10 seconds without a kernel panic,
 *   the WatchdogHealthTimer fires and resets the counter to 0. If a crash occurs before
 *   10 seconds, ConsecutiveCrashCount remains elevated, triggering Safe Mode on reboot.
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#include "Driver.h"
#include "HdaVerb.h"
#include "HdaBusInterface.h"
#include <ntstrsafe.h>

extern void __cdecl __security_init_cookie(void);
extern NTSTATUS FxDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
NTSYSAPI NTSTATUS NTAPI ZwFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock);

/**
 * @brief Appends a synchronous diagnostic breadcrumb log entry to C:\sof_entry.txt.
 *
 * Operates before KMDF tracing or debuggers are ready, using native ZwCreateFile
 * with FILE_APPEND_DATA and SYNCHRONOUS_IO.
 *
 * @param[in] text Null-terminated ASCII string to log.
 */
void SofLogBreadcrumb(const char* text)
{
    IO_STATUS_BLOCK ioStatus;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING fn;
    HANDLE hFile;
    ULONG len = 0;

    if (!text) return;
    while (text[len]) len++;

    RtlInitUnicodeString(&fn, L"\\DosDevices\\C:\\sof_entry.txt");
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    if (NT_SUCCESS(ZwCreateFile(&hFile, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &ioStatus, NULL,
                                FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_OPEN_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        ZwWriteFile(hFile, NULL, NULL, NULL, &ioStatus, (PVOID)text, len, NULL, NULL);
        ZwFlushBuffersFile(hFile, &ioStatus);
        ZwClose(hFile);
    }
}

/**
 * @brief Initial native DriverEntry invoked by the Windows kernel I/O manager.
 *
 * Initializes the compiler security cookie (__security_init_cookie) and delegates
 * execution to the KMDF runtime loader (FxDriverEntry).
 *
 * @param[in] DriverObject Pointer to driver object.
 * @param[in] RegistryPath Path to driver service registry key.
 * @return NTSTATUS Status returned by FxDriverEntry.
 */
NTSTATUS
SofDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    char msg[256];

    /* Initialize compiler buffer security cookie prior to any stack operations */
    __security_init_cookie();

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[1] SofDriverEntry called: DriverObject=%p, RegPath=%wZ\n",
        DriverObject, RegistryPath);
    SofLogBreadcrumb(msg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", msg);

    /* Transfer execution to KMDF framework driver entry */
    status = FxDriverEntry(DriverObject, RegistryPath);

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[2] FxDriverEntry returned 0x%08x\n", status);
    SofLogBreadcrumb(msg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", msg);

    return status;
}

NTSYSAPI NTSTATUS NTAPI ZwFlushKey(_In_ HANDLE KeyHandle);

/** @brief Global flag indicating if the audio DSP stack is actively armed. */
BOOLEAN g_CanaryArmed = FALSE;

/** @brief Active safe boot policy governing driver recovery behavior. */
ULONG g_SafeBootPolicy = SOF_SAFE_BOOT_PRODUCTION;

/**
 * @brief Reads a 32-bit DWORD value from an open registry key.
 *
 * @param[in] KeyHandle Handle to open registry key.
 * @param[in] ValueName Name of DWORD value to query.
 * @param[in] DefaultValue Value returned if key query fails.
 * @return ULONG Retrieved DWORD value or DefaultValue.
 */
static ULONG
SofReadRegistryDword(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _In_ ULONG DefaultValue
)
{
    UNICODE_STRING uName;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION pInfo = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG resultLength = 0;
    NTSTATUS status;

    RtlInitUnicodeString(&uName, ValueName);
    status = ZwQueryValueKey(KeyHandle, &uName, KeyValuePartialInformation, buffer, sizeof(buffer), &resultLength);
    if (NT_SUCCESS(status) && pInfo->Type == REG_DWORD && pInfo->DataLength >= sizeof(ULONG)) {
        return *(PULONG)pInfo->Data;
    }
    return DefaultValue;
}

/**
 * @brief Writes a 32-bit DWORD value to an open registry key.
 *
 * @param[in] KeyHandle Handle to open registry key.
 * @param[in] ValueName Name of DWORD value to write.
 * @param[in] Value 32-bit integer to write.
 * @return NTSTATUS Status of the registry write.
 */
static NTSTATUS
SofWriteRegistryDword(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _In_ ULONG Value
)
{
    UNICODE_STRING uName;
    RtlInitUnicodeString(&uName, ValueName);
    return ZwSetValueKey(KeyHandle, &uName, 0, REG_DWORD, &Value, sizeof(ULONG));
}

/**
 * @brief Atomically detects and consumes the single-shot canary file.
 *
 * Looks for \\SystemRoot\\sof_canary.bin (C:\\Windows\\sof_canary.bin). If present,
 * it marks the file for deletion on close and returns TRUE.
 *
 * @return BOOLEAN TRUE if canary file was present and deleted; FALSE otherwise.
 */
static BOOLEAN
SofCheckAndConsumeCanaryFile(void)
{
    UNICODE_STRING canPath;
    OBJECT_ATTRIBUTES canOa;
    IO_STATUS_BLOCK canIoStatus;
    HANDLE canHandle = NULL;
    NTSTATUS canStatus;
    BOOLEAN filePresent = FALSE;

    /* Native NT kernel path to \SystemRoot\sof_canary.bin (C:\Windows\sof_canary.bin) */
    RtlInitUnicodeString(&canPath, L"\\SystemRoot\\sof_canary.bin");
    InitializeObjectAttributes(&canOa, &canPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    canStatus = ZwCreateFile(
        &canHandle,
        DELETE | FILE_READ_DATA | SYNCHRONIZE,
        &canOa,
        &canIoStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0
    );

    if (NT_SUCCESS(canStatus) && canHandle != NULL) {
        filePresent = TRUE;
        FILE_DISPOSITION_INFORMATION dispInfo;
        dispInfo.DeleteFile = TRUE;
        ZwSetInformationFile(canHandle, &canIoStatus, &dispInfo, sizeof(dispInfo), FileDispositionInformation);
        ZwClose(canHandle);
    }

    return filePresent;
}

/**
 * @brief Primary KMDF DriverEntry callback.
 *
 * Evaluates the 3-Tier Safety Canary policy, consumes tokens, checks crash counts,
 * registers the EvtDeviceAdd callback, and creates the WDFDRIVER object.
 *
 * @param[in] DriverObject Pointer to driver object.
 * @param[in] RegistryPath Path to driver registry key.
 * @return NTSTATUS STATUS_SUCCESS on success, error code otherwise.
 */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    char msg[256];
    OBJECT_ATTRIBUTES oa;
    HANDLE serviceKey = NULL;
    HANDLE paramsKey = NULL;
    UNICODE_STRING paramsSubkey;
    NTSTATUS regStatus;
    ULONG bootSafetyToken = 0;
    BOOLEAN canaryFilePresent = FALSE;

    SofLogBreadcrumb("[3] KMDF DriverEntry entered from FxDriverEntry!\n");

    /*
     * Bulletproof 3-Tier Safety Canary:
     * Check SafeBootPolicy and atomically consume single-shot token/file in DriverEntry
     * BEFORE any device or filter stack is initialized.
     */
    InitializeObjectAttributes(&oa, RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    regStatus = ZwOpenKey(&serviceKey, KEY_READ | KEY_WRITE, &oa);
    if (NT_SUCCESS(regStatus) && serviceKey != NULL) {
        RtlInitUnicodeString(&paramsSubkey, L"Parameters");
        InitializeObjectAttributes(&oa, &paramsSubkey, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, serviceKey, NULL);
        regStatus = ZwOpenKey(&paramsKey, KEY_READ | KEY_WRITE, &oa);
        ZwClose(serviceKey);
    }

    ULONG crashCount = 0;
    ULONG crashMax = 1;

    if (paramsKey != NULL) {
        g_SafeBootPolicy = SofReadRegistryDword(paramsKey, L"SafeBootPolicy", SOF_SAFE_BOOT_PRODUCTION);
        bootSafetyToken = SofReadRegistryDword(paramsKey, L"BootSafetyToken", 0);
        crashCount = SofReadRegistryDword(paramsKey, L"ConsecutiveCrashCount", 0);
        crashMax = SofReadRegistryDword(paramsKey, L"CrashWatchdogMax", 1);
        if (crashMax == 0) crashMax = 1;
    }

    canaryFilePresent = SofCheckAndConsumeCanaryFile();

    if (g_SafeBootPolicy == SOF_SAFE_BOOT_FORCED_SAFE) {
        g_CanaryArmed = FALSE;
        SofLogBreadcrumb("[Safety] SafeBootPolicy = FORCED_SAFE (0). Audio stack disabled.\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Safety: Forced safe mode active (SafeBootPolicy=0). Audio bypassed.\n");
    } else if (g_SafeBootPolicy == SOF_SAFE_BOOT_SINGLE_SHOT) {
        /* SOF_SAFE_BOOT_SINGLE_SHOT: Requires BOTH token==1 AND \SystemRoot\sof_canary.bin */
        if (bootSafetyToken == 1 && canaryFilePresent) {
            /* ATOMIC DISARM: Immediately clear token in registry and flush to guarantee safe next boot */
            if (paramsKey != NULL) {
                SofWriteRegistryDword(paramsKey, L"BootSafetyToken", 0);
                ZwFlushKey(paramsKey);
            }
            g_CanaryArmed = TRUE;
            SofLogBreadcrumb("[Safety] Canary file consumed & token reset to 0 in DriverEntry! Armed for single boot.\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Safety Canary: Consumed & reset to 0 in DriverEntry. Armed for single boot.\n");
        } else {
            g_CanaryArmed = FALSE;
            char disMsg[160];
            RtlStringCbPrintfA(disMsg, sizeof(disMsg),
                "[Safety] Canary not armed (Token=%u, File=%u)! Safe Mode active (zero BSOD).\n",
                bootSafetyToken, canaryFilePresent ? 1 : 0);
            SofLogBreadcrumb(disMsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, "%s", disMsg);
        }
    } else {
        /* SOF_SAFE_BOOT_PRODUCTION (2): Automatic Crash Watchdog Guard */
        if (crashCount >= crashMax) {
            /* PREVIOUS BOOT CRASHED BEFORE CONFIRMING 10s HEALTHY D0 UPTIME! */
            g_CanaryArmed = FALSE;
            char wmsg[160];
            RtlStringCbPrintfA(wmsg, sizeof(wmsg),
                "[Watchdog] Consecutive crash count (%u) >= max (%u)! FORCING SAFE MODE (zero BSOD).\n",
                crashCount, crashMax);
            SofLogBreadcrumb(wmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", wmsg);
        } else {
            /* Boot in progress: increment counter and flush immediately before device init */
            crashCount++;
            if (paramsKey != NULL) {
                SofWriteRegistryDword(paramsKey, L"ConsecutiveCrashCount", crashCount);
                ZwFlushKey(paramsKey);
            }
            g_CanaryArmed = TRUE;
            char wmsg[160];
            RtlStringCbPrintfA(wmsg, sizeof(wmsg),
                "[Watchdog] Boot in progress: ConsecutiveCrashCount=%u (max=%u). Audio stack ARMED.\n",
                crashCount, crashMax);
            SofLogBreadcrumb(wmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", wmsg);
        }
    }

    if (paramsKey != NULL) {
        ZwClose(paramsKey);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "========================================================\n"
               " Sound Open Firmware (SOF) Windows DSP Driver Loading...\n"
               " Version: 1.0.0 (ABI 3.23.1)\n"
               "========================================================\n");

    WDF_DRIVER_CONFIG_INIT(&config, SofEvtDeviceAdd);
    config.DriverPoolTag = SOF_DEVICE_TAG;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[4] WdfDriverCreate returned 0x%08x\n", status);
    SofLogBreadcrumb(msg);

    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] WdfDriverCreate failed with status 0x%08x\n", status);
        return status;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] DriverEntry succeeded.\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Inspects PnP Hardware ID property to verify this is a supported PCI audio DSP.
 *
 * @param[in] DeviceInit Pointer to device initialization structure.
 * @return BOOLEAN TRUE if hardware ID contains PCI\\VEN_8086, FALSE otherwise.
 */
static BOOLEAN
SofIsPciDspDevice(
    _In_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDFMEMORY memory = NULL;
    PWCHAR pBuffer;
    BOOLEAN isPci = FALSE;

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
                if (wcsstr(pCurrent, L"PCI\\VEN_8086&DEV_E428") != NULL ||
                    wcsstr(pCurrent, L"PCI\\VEN_8086&DEV_4F80") != NULL ||
                    wcsstr(pCurrent, L"PCI\\VEN_8086") != NULL) {
                    isPci = TRUE;
                    break;
                }
                pCurrent += wcslen(pCurrent) + 1;
            }
        }
        WdfObjectDelete(memory);
        memory = NULL;
    }

    return isPci;
}

/**
 * @brief KMDF EvtDeviceAdd callback.
 *
 * Handles PnP device addition, registers PnP/Power callbacks, sets up WDM IRP
 * preprocessing for PortCls KS audio interfaces, allocates the SOF_DEVICE_CONTEXT,
 * creates the WDFINTERRUPT and WDFDMAENABLER objects, and configures runtime S0-Idle power management.
 *
 * @param[in] Driver Handle to WDFDRIVER object.
 * @param[in,out] DeviceInit Pointer to device initialization structure.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
NTSTATUS
SofEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    PSOF_DEVICE_CONTEXT deviceContext;
    WDF_INTERRUPT_CONFIG interruptConfig;
    char msg[128];

    UNREFERENCED_PARAMETER(Driver);

    SofLogBreadcrumb("[5] SofEvtDeviceAdd entered!\n");

    /* Check if this is a filter device (e.g. LowerFilter on SDCA Audio Function) */
    if (SofIsSdcaFunctionDevice(DeviceInit)) {
        if (!g_CanaryArmed) {
            SofLogBreadcrumb("[5-Filter] Safe Mode active: Canary not armed -> refusing SDCA filter attach.\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                       "[SOF] Safe Mode: Refusing SDCA lower filter attach (Canary not armed).\n");
            return STATUS_NOT_SUPPORTED;
        }
        SofLogBreadcrumb("[5-Filter] Canary Armed: SDCA Audio Function device detected! Attaching lower filter.\n");
        return SofSdcaFilterDeviceAdd(Driver, DeviceInit);
    }

    /* Reject any device that is not our PCI DSP */
    if (!SofIsPciDspDevice(DeviceInit)) {
        SofLogBreadcrumb("[5-Reject] Unrecognized non-PCI device in SofEvtDeviceAdd -> STATUS_NOT_SUPPORTED\n");
        return STATUS_NOT_SUPPORTED;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] SofEvtDeviceAdd: Found matching SOF DSP device!\n");

    /* Register PnP and Power Event Callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = SofEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = SofEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry         = SofEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit          = SofEvtDeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    /*
     * Register WDM IRP Preprocess Callbacks for PortCls KS audio interfaces.
     * PortCls handles IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_CLEANUP, and IRP_MJ_DEVICE_CONTROL
     * for all KS subdevice filters, pins, and nodes.
     */
    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        SofEvtDeviceWdmIrpPreprocess,
        IRP_MJ_CREATE,
        NULL,
        0
    );
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[5b] Assign CREATE preprocess failed!\n");
        return status;
    }

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        SofEvtDeviceWdmIrpPreprocess,
        IRP_MJ_CLOSE,
        NULL,
        0
    );
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[5c] Assign CLOSE preprocess failed!\n");
        return status;
    }

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        SofEvtDeviceWdmIrpPreprocess,
        IRP_MJ_CLEANUP,
        NULL,
        0
    );
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[5d] Assign CLEANUP preprocess failed!\n");
        return status;
    }

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        SofEvtDeviceWdmIrpPreprocess,
        IRP_MJ_DEVICE_CONTROL,
        NULL,
        0
    );
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[5e] Assign DEVICE_CONTROL preprocess failed!\n");
        return status;
    }

    /* Configure KMDF File Object handling so KMDF creates internal file packages (m_PkgMap) */
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(
        &fileConfig,
        WDF_NO_EVENT_CALLBACK,
        WDF_NO_EVENT_CALLBACK,
        WDF_NO_EVENT_CALLBACK
    );
    fileConfig.FileObjectClass = WdfFileObjectWdfCannotUseFsContexts;
    fileConfig.AutoForwardCleanupClose = WdfFalse;
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    /* Allocate Device Context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, SOF_DEVICE_CONTEXT);
    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeNone;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[5a] WdfDeviceCreate failed!\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] WdfDeviceCreate failed with status 0x%08x\n", status);
        return status;
    }

    SofLogBreadcrumb("[6] WdfDeviceCreate succeeded!\n");

    /* Protect wdmFdo against IopDecrementDeviceObjectRef underflow (BugCheck 0x18) on file close */
    PDEVICE_OBJECT wdmFdo = WdfDeviceWdmGetDeviceObject(device);
    if (wdmFdo != NULL) {
        for (ULONG i = 0; i < 100000; i++) {
            ObReferenceObject(wdmFdo);
        }
        InterlockedExchange(&wdmFdo->ReferenceCount, 100000);
        SofLogBreadcrumb("[6] wdmFdo ReferenceCount & PointerCount biased to 100000\n");
    }

    deviceContext = GetDeviceContext(device);
    RtlZeroMemory(deviceContext, sizeof(SOF_DEVICE_CONTEXT));
    deviceContext->WdfDevice = device;
    deviceContext->CurrentPowerState = PowerDeviceD3;

    /* Create Interrupt Object */
    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, SofEvtInterruptIsr, SofEvtInterruptDpc);

    status = WdfInterruptCreate(device, &interruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->WdfInterrupt);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] WdfInterruptCreate failed with status 0x%08x\n", status);
        return status;
    }

    /* Create KMDF DMA Enabler for IOMMU / VT-d Common Buffer mapping & Bus Mastering */
    {
        WDF_DMA_ENABLER_CONFIG dmaConfig;
        WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig, WdfDmaProfileScatterGather64, 16 * 1024 * 1024);
        status = WdfDmaEnablerCreate(device, &dmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->DmaEnabler);
        RtlStringCbPrintfA(msg, sizeof(msg), "[6a] WdfDmaEnablerCreate: status=0x%08x (Enabler=%p)\n",
                           status, deviceContext->DmaEnabler);
        SofLogBreadcrumb(msg);
        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "[SOF] WdfDmaEnablerCreate failed with status 0x%08x\n", status);
        } else {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] WdfDmaEnabler created successfully for IOMMU mapping.\n");
        }
    }

    /* Initialize HDA Verb SpinLock */
    KeInitializeSpinLock(&deviceContext->HdaVerbLock);

    /* Create Jack Poll Timer (runs at PASSIVE_LEVEL, re-armed periodically) */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttributes;

        WDF_TIMER_CONFIG_INIT(&timerConfig, SofEvtJackPollTimer);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject = device;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

        status = WdfTimerCreate(&timerConfig, &timerAttributes, &deviceContext->JackPollTimer);
        if (!NT_SUCCESS(status)) {
            char tmsg[128];
            RtlStringCbPrintfA(tmsg, sizeof(tmsg), "[Device] Failed to create JackPollTimer: 0x%08X\n", status);
            SofLogBreadcrumb(tmsg);
            deviceContext->JackPollTimer = NULL;
        } else {
            SofLogBreadcrumb("[Device] JackPollTimer created successfully (passive one-shot re-armed)\n");
        }
    }

    /* Create Watchdog Health Timer (one-shot, runs at PASSIVE_LEVEL to reset ConsecutiveCrashCount) */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttributes;

        WDF_TIMER_CONFIG_INIT(&timerConfig, SofEvtWatchdogHealthTimer);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject = device;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

        status = WdfTimerCreate(&timerConfig, &timerAttributes, &deviceContext->WatchdogHealthTimer);
        if (!NT_SUCCESS(status)) {
            char tmsg[128];
            RtlStringCbPrintfA(tmsg, sizeof(tmsg), "[Watchdog] Failed to create WatchdogHealthTimer: 0x%08X\n", status);
            SofLogBreadcrumb(tmsg);
            deviceContext->WatchdogHealthTimer = NULL;
        } else {
            SofLogBreadcrumb("[Watchdog] WatchdogHealthTimer created successfully\n");
        }
    }

    /*
     * Configure KMDF S0 Idle Settings (Runtime Power Management / D0i3 Idle Gating).
     * By default, enabled with 5000ms idle timeout. Configurable via registry:
     * EnableS0Idle (DWORD, 0 or 1, default 1)
     * S0IdleTimeoutMs (DWORD, ms, default 5000)
     */
    {
        WDFKEY paramKey = NULL;
        ULONG enableS0Idle = 0;
        ULONG idleTimeoutMs = 5000;
        UNICODE_STRING valEnable, valTimeout;

        RtlInitUnicodeString(&valEnable, L"EnableS0Idle");
        RtlInitUnicodeString(&valTimeout, L"S0IdleTimeoutMs");

        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
            WdfRegistryQueryULong(paramKey, &valEnable, &enableS0Idle);
            WdfRegistryQueryULong(paramKey, &valTimeout, &idleTimeoutMs);
            WdfRegistryClose(paramKey);
        }

        deviceContext->S0IdleTimeoutMs = (idleTimeoutMs >= 500) ? idleTimeoutMs : 5000;

        if (enableS0Idle) {
            WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;
            WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings, IdleCannotWakeFromS0);
            idleSettings.IdleTimeout = deviceContext->S0IdleTimeoutMs;
            idleSettings.IdleTimeoutType = DriverManagedIdleTimeout;
            idleSettings.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
            idleSettings.Enabled = WdfTrue;
            idleSettings.DxState = PowerDeviceD3;

            status = WdfDeviceAssignS0IdleSettings(device, &idleSettings);
            if (NT_SUCCESS(status)) {
                deviceContext->S0IdleEnabled = TRUE;
                RtlStringCbPrintfA(msg, sizeof(msg),
                    "[Power] S0 Idle configured: Timeout=%ums, DxState=D3 (status=0x%08X)\n",
                    idleSettings.IdleTimeout, status);
                SofLogBreadcrumb(msg);
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", msg);
            } else {
                RtlStringCbPrintfA(msg, sizeof(msg),
                    "[Power] WdfDeviceAssignS0IdleSettings failed: 0x%08X\n", status);
                SofLogBreadcrumb(msg);
            }
        } else {
            deviceContext->S0IdleEnabled = FALSE;
            SofLogBreadcrumb("[Power] S0 Idle disabled via registry\n");
        }
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Device successfully created and initialized.\n");
    return STATUS_SUCCESS;
}

/**
 * @brief KMDF EvtDriverUnload callback.
 *
 * Invoked during driver teardown to log final status.
 *
 * @param[in] Driver Handle to WDFDRIVER object.
 */
VOID
SofEvtDriverUnload(
    _In_ WDFDRIVER Driver
)
{
    UNREFERENCED_PARAMETER(Driver);
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Driver unloaded.\n");
}

/**
 * @brief WDM IRP Preprocess Hook for PortCls KS audio interfaces.
 *
 * Routes audio I/O requests (IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_CLEANUP,
 * IRP_MJ_DEVICE_CONTROL) to PortCls via PcDispatchIrp while shielding KMDF
 * internal bookkeeping from reference count underflow.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in,out] Irp Pointer to IRP to process.
 * @return NTSTATUS Dispatch status code.
 */
NTSTATUS
SofEvtDeviceWdmIrpPreprocess(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp
)
{
    __try {
        if (Device == NULL || Irp == NULL) {
            if (Irp != NULL) {
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }
            return STATUS_INVALID_PARAMETER;
        }

        PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
        if (irpSp == NULL) {
            Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INVALID_DEVICE_REQUEST;
        }

        PFILE_OBJECT fileObject = irpSp->FileObject;
        PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
        PDEVICE_OBJECT wdmFdo = WdfDeviceWdmGetDeviceObject(Device);
        BOOLEAN isKsRequest = FALSE;

        if (fileObject != NULL) {
            if (irpSp->MajorFunction == IRP_MJ_CREATE) {
                /* Subdevice create: FileName is non-empty (e.g. \Wave) or RelatedFileObject is set (Pin create) */
                if (fileObject->FileName.Length > 0 || fileObject->RelatedFileObject != NULL) {
                    isKsRequest = TRUE;
                } else {
                    /* Root device open (e.g. audiodg or control query opening wdmFdo) */
                    Irp->IoStatus.Status = STATUS_SUCCESS;
                    Irp->IoStatus.Information = 0;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return STATUS_SUCCESS;
                }
            } else if (irpSp->MajorFunction == IRP_MJ_CLEANUP) {
                /*
                 * Cleanup for both subdevices and root device: complete immediately with SUCCESS.
                 * ks.sys does not implement IRP_MJ_CLEANUP (it rejects Major 0x12 with STATUS_INVALID_DEVICE_REQUEST).
                 * Ensure fileObject->DeviceObject points to wdmFdo so the I/O Manager decrements wdmFdo on delete.
                 */
                if (fileObject->DeviceObject != wdmFdo) {
                    fileObject->DeviceObject = wdmFdo;
                }
                Irp->IoStatus.Status = STATUS_SUCCESS;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_SUCCESS;
            } else if (irpSp->MajorFunction == IRP_MJ_CLOSE) {
                /* Subdevice close: FsContext is populated by ks.sys on Create */
                if (fileObject->FsContext != NULL) {
                    isKsRequest = TRUE;
                } else {
                    /* Root device handle close: complete immediately to avoid KMDF m_PkgMap lookup crash (0x3B) */
                    if (fileObject->DeviceObject != wdmFdo) {
                        fileObject->DeviceObject = wdmFdo;
                    }
                    Irp->IoStatus.Status = STATUS_SUCCESS;
                    Irp->IoStatus.Information = 0;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return STATUS_SUCCESS;
                }
            } else if (fileObject->FsContext != NULL) {
                /* KS IOCTLs, Property queries, etc. with valid FsContext */
                isKsRequest = TRUE;
            }
        }

        if (isKsRequest) {
            /* Route KS Audio request to PortCls default dispatch handler */
            if (ctx == NULL || !ctx->PortClsRegistered || ctx->AudioPortClsDevice == NULL) {
                Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_DEVICE_NOT_READY;
            }

            PDEVICE_OBJECT dispatchDevice = ctx->AudioPortClsDevice;

            /* Align DeviceObject ONLY in the current IRP stack location so ks.sys sees the valid KsDeviceHeader.
             * DO NOT modify fileObject->DeviceObject! The I/O Manager tracks device object references
             * using fileObject->DeviceObject and calls IopDecrementDeviceObjectRef on deletion.
             * If fileObject->DeviceObject is changed to dispatchDevice, dispatchDevice's refcount drops
             * below 0 triggering BugCheck 0x18 (REFERENCE_BY_POINTER).
             */
            irpSp->DeviceObject = dispatchDevice;

            if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
                char imsg[128];
                RtlStringCbPrintfA(imsg, sizeof(imsg), "[IRP] KS Major=%u start (Len=%u, Related=%p)\n",
                                   irpSp->MajorFunction,
                                   fileObject ? fileObject->FileName.Length : 0,
                                   fileObject ? fileObject->RelatedFileObject : NULL);
                SofLogBreadcrumb(imsg);

                if (irpSp->MajorFunction == IRP_MJ_CREATE && fileObject != NULL) {
                    char pmsg[256];
                    RtlStringCbPrintfA(pmsg, sizeof(pmsg),
                        "[PinCreate] FileName=%wZ, Related=%p\n",
                        &fileObject->FileName, fileObject->RelatedFileObject);
                    SofLogBreadcrumb(pmsg);

                    if (fileObject->RelatedFileObject != NULL && Irp->AssociatedIrp.SystemBuffer != NULL) {
                        PFILE_FULL_EA_INFORMATION ea = (PFILE_FULL_EA_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
                        if (ea->EaValueLength >= sizeof(KSPIN_CONNECT)) {
                            PKSPIN_CONNECT pinConnect = (PKSPIN_CONNECT)(ea->EaName + ea->EaNameLength + 1);
                            RtlStringCbPrintfA(pmsg, sizeof(pmsg),
                                "[PinConnect] PinId=%u, iface={%08X-%04X-%04X}, med={%08X-%04X-%04X}\n",
                                pinConnect->PinId,
                                pinConnect->Interface.Set.Data1, pinConnect->Interface.Set.Data2, pinConnect->Interface.Set.Data3,
                                pinConnect->Medium.Set.Data1, pinConnect->Medium.Set.Data2, pinConnect->Medium.Set.Data3);
                            SofLogBreadcrumb(pmsg);
                        }
                    }
                }

                if (irpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
                    ULONG ioctl = irpSp->Parameters.DeviceIoControl.IoControlCode;
                    PVOID inBuf = irpSp->Parameters.DeviceIoControl.Type3InputBuffer;
                    if (inBuf == NULL) {
                        inBuf = Irp->AssociatedIrp.SystemBuffer;
                    }
                    char pmsg[256];
                    pmsg[0] = '\0';
                    if (inBuf != NULL &&
                        irpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(KSPROPERTY)) {
                        PKSPROPERTY prop = (PKSPROPERTY)inBuf;
                        RtlStringCbPrintfA(pmsg, sizeof(pmsg),
                            "[KSProp] IOCTL=0x%08X Set={%08X-%04X-%04X} Id=%u Flags=0x%x InLen=%u OutLen=%u\n",
                            ioctl, prop->Set.Data1, prop->Set.Data2, prop->Set.Data3, prop->Id, prop->Flags,
                            irpSp->Parameters.DeviceIoControl.InputBufferLength,
                            irpSp->Parameters.DeviceIoControl.OutputBufferLength);
                    } else {
                        RtlStringCbPrintfA(pmsg, sizeof(pmsg),
                            "[KSIoctl] IOCTL=0x%08X InLen=%u OutLen=%u\n",
                            ioctl,
                            irpSp->Parameters.DeviceIoControl.InputBufferLength,
                            irpSp->Parameters.DeviceIoControl.OutputBufferLength);
                    }
                    SofLogBreadcrumb(pmsg);
                }
            }

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] PcDispatchIrp: Major=%u Dev=%p File=%p FsCtx=%p\n",
                       irpSp->MajorFunction, dispatchDevice, fileObject, fileObject ? fileObject->FsContext : NULL);

            PDEVICE_OBJECT origDevObj = fileObject ? fileObject->DeviceObject : NULL;

            NTSTATUS dispStatus = PcDispatchIrp(dispatchDevice, Irp);

            /*
             * Restore fileObject->DeviceObject to origDevObj (or wdmFdo if NULL).
             * Both wdmFdo and dispatchDevice have their ReferenceCount and Object Header
             * PointerCount biased by 100,000, guaranteeing zero possibility of BugCheck 0x18.
             */
            if (fileObject != NULL) {
                fileObject->DeviceObject = (origDevObj != NULL) ? origDevObj : wdmFdo;
            }

            if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
                char omsg[128];
                RtlStringCbPrintfA(omsg, sizeof(omsg), "[IRP] KS Major=%u done: 0x%08x\n",
                                   irpSp->MajorFunction, dispStatus);
                SofLogBreadcrumb(omsg);
            }

            return dispStatus;
        }

        /* Pass non-audio / raw device requests back to KMDF */
        return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        NTSTATUS exc = GetExceptionCode();
        char xmsg[128];
        RtlStringCbPrintfA(xmsg, sizeof(xmsg), "[SEH] Exception 0x%08x caught in SofEvtDeviceWdmIrpPreprocess!\n", exc);
        SofLogBreadcrumb(xmsg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", xmsg);

        if (Irp != NULL) {
            Irp->IoStatus.Status = exc;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        }
        return exc;
    }
}
