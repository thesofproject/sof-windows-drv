/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Ipc.h
 * @brief Inter-Processor Communication (IPC) Protocol Header for Sound Open Firmware.
 *
 * @details
 * Defines data structures, register abstractions, and API prototypes for host-to-DSP
 * inter-processor messaging on Intel cAVS (1.5 - 2.5), Intel ACE (1.0 - 3.0), and
 * compatible architectures running the IPC4 protocol.
 *
 * IPC messaging utilizes a doorbell and mailbox register architecture:
 *  - Doorbells: Initiator and Target Request/Acknowledge registers (HIPCIDR, HIPCIDD,
 *    HIPCIDA, HIPCTDR, HIPCTDD, HIPCTDA) mapped in DSP SHIM MMIO (BAR4).
 *  - Mailboxes: Shared SRAM windows in DSP BAR4 for transmitting large message payloads
 *    (Uplink: DSP->Host; Downlink: Host->DSP).
 *  - Interrupts: Multi-level hierarchy with top-level DSP IRQINTEN/IRQSTS, second-level
 *    HfHIPCIE, and channel-level HIPCCTL.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_IPC_H__
#define __SOF_IPC_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#include <windows.h>
#endif
#include <sof/sof_osal.h>
#include <sof/ipc4/header.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct _SOF_DEVICE_CONTEXT *PSOF_DEVICE_CONTEXT;

/**
 * @struct _SOF_IPC_MESSAGE
 * @brief Container for IPC4 protocol messages and associated mailbox payloads.
 *
 * Transmitted across the DSP doorbell registers and shared SRAM mailbox windows.
 * The Primary and Extension DWORDs contain command IDs, module instance IDs,
 * status codes, and bitflags defined by the Intel IPC4 specification.
 */
typedef struct _SOF_IPC_MESSAGE {
    /**
     * @brief Primary 32-bit IPC header.
     * Maps to HIPCIDR (Host Initiator Doorbell) or HIPCTDR (DSP Target Doorbell).
     * Bit 31 is the hardware BUSY bit.
     */
    ULONG   Primary;

    /**
     * @brief Extension 32-bit IPC header.
     * Maps to HIPCIDD (Host Initiator Extension) or HIPCTDD (DSP Target Extension).
     */
    ULONG   Extension;

    /**
     * @brief Pointer to optional payload buffer in system memory.
     * Copied to the Downlink Mailbox on transmission or from the Uplink Mailbox on reply.
     */
    PVOID   Payload;

    /**
     * @brief Size of the payload buffer in bytes (must not exceed mailbox window size, typically 4KB).
     */
    ULONG   PayloadSize;
} SOF_IPC_MESSAGE, *PSOF_IPC_MESSAGE;

/* IPC Lifecycle and Control */

/**
 * @brief Initializes the IPC communication subsystem and allocates state buffers.
 *
 * Allocates the non-paged reply buffer, initializes synchronization spinlocks,
 * and sets up synchronization events for synchronous command dispatch.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return STATUS_SUCCESS on success, or STATUS_INSUFFICIENT_RESOURCES on allocation failure.
 */
NTSTATUS SofIpcInitialize(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Cleans up IPC subsystem resources during driver teardown.
 *
 * Frees non-paged reply buffers and resets state flags.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 */
VOID SofIpcCleanup(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Unmasks and enables IPC interrupts in DSP SHIM and hardware control registers.
 *
 * Programs top-level IRQINTEN, second-level HfHIPCIE (on ACE platforms), and channel-level
 * HIPCCTL to enable BUSY and DONE interrupt notifications from the DSP.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID SofIpcEnableInterrupts(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Masks and disables all IPC interrupts in DSP hardware.
 *
 * Clears BUSY and DONE interrupt enable bits in HIPCCTL and upstream interrupt mask
 * registers to prevent interrupt generation during power-down or reset.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID SofIpcDisableInterrupts(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/* ISR / DPC Handlers */

/**
 * @brief Interrupt Service Routine (ISR) check and quick-dismiss for IPC interrupts.
 *
 * Evaluates whether an IPC interrupt is pending (DSP TDR BUSY or IDA DONE asserted).
 * On active interrupts, masks HIPCCTL BUSY/DONE immediately to eliminate line-based
 * interrupt storms before returning TRUE to request DPC scheduling.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return TRUE if an IPC interrupt was acknowledged and DPC is required; FALSE otherwise.
 */
BOOLEAN SofIpcIsrCheck(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Deferred Procedure Call (DPC) worker for processing IPC events.
 *
 * Processes completed host transmissions (IDA DONE), unsolicited notifications from DSP
 * (e.g. FW_READY, EXCEPTION_CAUGHT), and synchronous command replies (TDR BUSY).
 * Reads payload data from the Uplink mailbox and signals waiting threads.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 */
VOID SofIpcDpcHandler(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/* Synchronous IPC Message Dispatch */

/**
 * @brief Synchronously sends an IPC4 message to the DSP and awaits the response.
 *
 * Writes message payload to the Downlink Mailbox, programs Extension (HIPCIDD) and
 * Primary (HIPCIDR) with the BUSY bit asserted, and blocks on a synchronization event
 * until a reply is received or the specified timeout expires. Includes fallback hardware
 * register polling to prevent hangs if an interrupt was dropped under high system load.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @param[in]     Msg Pointer to the IPC message to transmit.
 * @param[out]    Reply Optional pointer to receive the reply headers and payload.
 * @param[in]     TimeoutMs Timeout duration in milliseconds.
 * @return STATUS_SUCCESS on successful ACK and zero status; STATUS_IO_TIMEOUT on timeout;
 *         STATUS_DEVICE_BUSY if transmitter is locked; STATUS_UNSUCCESSFUL on FW error status.
 */
NTSTATUS SofIpcSendMessage(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PSOF_IPC_MESSAGE Msg,
    _Out_opt_ PSOF_IPC_MESSAGE Reply,
    _In_ ULONG TimeoutMs
);

/* Status Reading */

/**
 * @brief Reads the DSP Firmware Status register (FWSTATUS / HFDSC).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @return 32-bit hardware register value, or 0 if unmapped.
 */
ULONG SofReadFwStatus(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/**
 * @brief Reads the DSP ROM Status register (ROMSTATUS / HFDSC).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @return 32-bit hardware register value, or 0 if unmapped.
 */
ULONG SofReadRomStatus(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

/* DSP Core Dump & Exception Telemetry */

/**
 * @brief Captures a full post-mortem core dump and diagnostic log following a DSP crash or timeout.
 *
 * Reads hardware status registers, IPC doorbell registers, extended ROM registers, and the full
 * contents of both Uplink and Downlink mailboxes (8KB total). Parses Xtensa architecture exception
 * frames (EXCCAUSE, EXCVADDR, EPC1, PS) if present, logs telemetry to breadcrumbs and kernel debugger,
 * and writes binary (.bin) and human-readable (.txt) crash dump files to C:\.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @param[in] Reason Diagnostic string describing the triggering condition (e.g. "IPC_TIMEOUT").
 */
VOID SofDspDumpCore(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ const char *Reason
);

/* Zephyr FW Logging / ADSP Mtrace */

/**
 * @brief Reads and drains newly arrived Zephyr firmware logs from the DSP SRAM Debug Window.
 *
 * Implements host-side processing of SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS notifications
 * generated by the Zephyr ADSP Mtrace backend (CONFIG_LOG_BACKEND_ADSP_MTRACE).
 *
 * @param[in,out] Ctx  Pointer to the SOF device context.
 * @param[in]     Core Zero-based DSP core index (0..3).
 */
VOID SofIpc4DrainFwLogs(
    _Inout_ PSOF_DEVICE_CONTEXT Ctx,
    _In_    ULONG               Core
);

/**
 * @brief Configures and enables DSP firmware Zephyr logging over IPC4.
 *
 * Transmits system time synchronization (SOF_IPC4_FW_PARAM_SYSTEM_TIME) and log priority
 * configuration (SOF_IPC4_FW_PARAM_ENABLE_LOGS) to the Base FW module.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return NTSTATUS STATUS_SUCCESS if firmware logging was successfully armed.
 */
NTSTATUS SofIpc4EnableFwLogs(
    _Inout_ PSOF_DEVICE_CONTEXT Ctx
);

#ifdef __cplusplus
}
#endif

#endif /* __SOF_IPC_H__ */
