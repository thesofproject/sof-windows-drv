/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file PortClsAdapter.h
 * @brief PortCls COM WaveRT Miniport Adapter Header for Sound Open Firmware.
 *
 * @details
 * Declares exported C interfaces for instantiating PortCls subdevices, registering
 * IMiniportWaveRT and IMiniportWaveRTStreamNotification COM miniports with Windows
 * PortCls (`portcls.sys`), and propagating jack presence change notifications.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_PORTCLS_ADAPTER_H__
#define __SOF_PORTCLS_ADAPTER_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <portcls.h>
#include "Device.h"
#include "WaveRt.h"
#include "Topo.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers PortCls WaveRT subdevices with the Windows Audio Subsystem.
 *
 * Calls PcNewPort (CLSID_PortWaveRT) and creates the custom miniport object (CMiniportWaveRTSof).
 * Binds them together via PcRegisterSubdevice under the "Wave" symbolic name.
 *
 * @param[in] Device WDFDEVICE handle.
 * @param[in] Ctx    Pointer to SOF device context.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofRegisterPortClsSubdevices(
    _In_ WDFDEVICE Device,
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Unregisters PortCls WaveRT subdevices during driver teardown.
 *
 * Releases references to PortCls port and miniport objects.
 *
 * @param[in] Device WDFDEVICE handle.
 * @param[in] Ctx    Pointer to SOF device context.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofUnregisterPortClsSubdevices(
    _In_ WDFDEVICE Device,
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Notifies Windows PortCls of a jack presence change on the specified pin.
 *
 * Uses IPortEvents::GenerateEventList to signal KSEVENT_PINCAPS_JACKINFOCHANGE to
 * notify Windows Audio Endpoint Builder that an audio jack was inserted or removed.
 *
 * @param[in] Ctx   Pointer to SOF device context.
 * @param[in] PinId Hardware pin index that experienced the jack event.
 */
void SofPortClsNotifyJackPresence(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PinId
);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_MODE */

#endif /* __SOF_PORTCLS_ADAPTER_H__ */
