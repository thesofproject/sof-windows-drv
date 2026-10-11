/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file HdaBusInterface.h
 * @brief High Definition Audio Bus Interface (HDAUDIO_BUS_INTERFACE_V2) Header.
 *
 * @details
 * Declares function table initializers and child device creation routines for
 * exposing the standard Microsoft HDAUDIO_BUS_INTERFACE_V2 interface to child
 * audio codec drivers (e.g. Realtek HDAudio.sys or third-party audio function drivers).
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_HDA_BUS_INTERFACE_H__
#define __SOF_HDA_BUS_INTERFACE_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <hdaudio.h>
#include "Device.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Populates the standard Microsoft HDAUDIO_BUS_INTERFACE_V2 function pointer table.
 *
 * Exposes TransferCodecVerbs, AllocateCaptureDmaEngine, AllocateRenderDmaEngine,
 * ChangeBandwidthAllocation, FreeDmaEngine, SetDmaEngineState, and buffer management
 * routines to child codec drivers.
 *
 * @param[in]  Ctx          Pointer to SOF device context.
 * @param[out] BusInterface Pointer to caller-allocated HDAUDIO_BUS_INTERFACE_V2 structure.
 */
VOID SofHdaBusInitInterface(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _Out_ PHDAUDIO_BUS_INTERFACE_V2 BusInterface
);

/**
 * @brief Creates a KMDF child Physical Device Object (PDO) for the detected HD-A codec.
 *
 * Configures hardware IDs (HDAUDIO\\FUNC_01&VEN_xxxx&DEV_xxxx), compatible IDs,
 * device relations, and sets up QueryInterface handling for GUID_HDAUDIO_BUS_INTERFACE_V2.
 *
 * @param[in] Ctx Pointer to SOF device context.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofHdaCreateCodecChildPdo(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_MODE */

#endif /* __SOF_HDA_BUS_INTERFACE_H__ */
