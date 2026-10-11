/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Driver.h
 * @brief Driver Entry, SafeBootPolicy Engine, and Global Declarations for the SOF Windows Driver.
 *
 * Declares the KMDF driver entry point, PnP device addition, unload callbacks,
 * WDM IRP preprocess hooks for PortCls audio streaming, and the 3-Tier Safety Canary.
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#ifndef __SOF_DRIVER_H__
#define __SOF_DRIVER_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <portcls.h>
#include "Device.h"

/**
 * @brief Driver entry point called by the OS loader (FxDriverEntry trampoline).
 *
 * @param[in] DriverObject Pointer to the DRIVER_OBJECT created by the I/O Manager.
 * @param[in] RegistryPath Pointer to the driver's Services registry key path.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
DRIVER_INITIALIZE SofDriverEntry;

/**
 * @brief Standard KMDF DriverEntry callback invoked by WDF.
 *
 * @param[in] DriverObject Pointer to the DRIVER_OBJECT.
 * @param[in] RegistryPath Pointer to the driver registry path.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
DRIVER_INITIALIZE DriverEntry;

/**
 * @brief KMDF EvtDeviceAdd callback invoked when PnP discovers an audio DSP.
 *
 * @param[in] Driver Handle to the WDFDRIVER object.
 * @param[in,out] DeviceInit Pointer to the PWDFDEVICE_INIT structure.
 * @return NTSTATUS STATUS_SUCCESS on success, error code otherwise.
 */
EVT_WDF_DRIVER_DEVICE_ADD SofEvtDeviceAdd;

/**
 * @brief KMDF EvtDriverUnload callback invoked upon driver teardown.
 *
 * @param[in] Driver Handle to the WDFDRIVER object.
 */
EVT_WDF_DRIVER_UNLOAD SofEvtDriverUnload;

/**
 * @brief WDM IRP Preprocess callback for PortCls KS audio interfaces.
 *
 * Intercepts IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_CLEANUP, and IRP_MJ_DEVICE_CONTROL,
 * routing KS filter/pin/property requests to PortCls (PcDispatchIrp) while preserving
 * KMDF internal object lifecycle.
 *
 * @param[in] Device Handle to the WDFDEVICE object.
 * @param[in,out] Irp Pointer to the I/O Request Packet.
 * @return NTSTATUS Status of the dispatched IRP.
 */
EVT_WDFDEVICE_WDM_IRP_PREPROCESS SofEvtDeviceWdmIrpPreprocess;

/**
 * @brief Timer callback executed 10 seconds after D0Entry to reset the crash counter.
 *
 * @param[in] Timer Handle to the WDFTIMER object.
 */
EVT_WDF_TIMER SofEvtWatchdogHealthTimer;

#else
#include <windows.h>
#include "Device.h"
#endif

/* =========================================================================
 * 3-Tier SafeBootPolicy Engine Definitions
 * ========================================================================= */

/**
 * @brief SafeBootPolicy 0: Forced Safe Mode.
 * Audio hardware programming and PortCls stack are bypassed; driver acts as a passive stub.
 */
#define SOF_SAFE_BOOT_FORCED_SAFE 0

/**
 * @brief SafeBootPolicy 1: Single-Shot Canary Token.
 * Requires BOTH BootSafetyToken=1 and canary file (\\SystemRoot\\sof_canary.bin).
 * Automatically resets token to 0 on boot to guarantee safe fallback on crash.
 */
#define SOF_SAFE_BOOT_SINGLE_SHOT 1

/**
 * @brief SafeBootPolicy 2: Production Watchdog.
 * Automatically tracks consecutive crashes in registry (ConsecutiveCrashCount).
 * If crash occurs before 10s health timer expires, forces Safe Mode on next boot.
 */
#define SOF_SAFE_BOOT_PRODUCTION  2

/** @brief Global flag indicating whether the audio stack is armed (TRUE) or in Safe Mode (FALSE). */
extern BOOLEAN g_CanaryArmed;

/** @brief Global policy setting governing driver boot safety behavior. */
extern ULONG g_SafeBootPolicy;

/**
 * @brief Appends a diagnostic breadcrumb log entry to C:\sof_entry.txt.
 *
 * Used for early boot diagnostics and panic tracing before kernel debuggers are online.
 *
 * @param[in] text Null-terminated ASCII string to log.
 */
void SofLogBreadcrumb(const char* text);

#endif /* __SOF_DRIVER_H__ */
