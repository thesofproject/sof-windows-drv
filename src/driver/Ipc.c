/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Ipc.c
 * @brief Inter-Processor Communication (IPC) Protocol Handler for Sound Open Firmware.
 *
 * @details
 * Implements low-level register programming, shared SRAM mailbox data transfer,
 * doorbell signaling, interrupt mitigation, and IPC4 protocol dispatch between the
 * Windows host kernel and Intel cAVS / ACE Audio DSP microarchitectures.
 *
 * Hardware Mechanisms:
 *  1. Doorbell Registers:
 *     - Host Initiator: HIPCIDR (Primary Command + BUSY bit), HIPCIDD (Extension Data),
 *       HIPCIDA (DSP Acknowledgement + DONE bit).
 *     - DSP Target: HIPCTDR (Incoming Command/Notification + BUSY bit), HIPCTDD (Extension Data),
 *       HIPCTDA (Host Acknowledgement + DONE bit).
 *  2. Shared Mailbox Windows (DSP SRAM):
 *     - Downlink Mailbox (Host -> DSP): Used for transmitting parameter blocks, pipeline configs,
 *       and large data structures across BAR4.
 *     - Uplink Mailbox (DSP -> Host): Used for receiving response payloads, telemetry data, and
 *       firmware exception dumps.
 *  3. Interrupt Routing Hierarchy:
 *     - Top-Level IRQ: Interrupt IP Pointer register (MTL_HFINTIPPTR) points to top-level
 *       DSP IRQINTEN / IRQSTS registers.
 *     - Second-Level IRQ: HfHIPCIE (Host Interface IPC Interrupt Enable).
 *     - Channel-Level IRQ: HIPCCTL (Control register masking BUSY and DONE interrupt generation).
 *  4. Telemetry and Post-Mortem Debugging:
 *     - Captures DSP Core 0 Status (HFDSC), Core 0 Error (HFDEC), Extended ROM status registers,
 *       and parses Xtensa architecture exception frames (EXCCAUSE, EXCVADDR, EPC1, PS).
 *     - Generates binary (.bin) and structured text (.txt) coredumps directly to disk.
 * Copyright 2026 The SOF Authors
 */

#include "Device.h"
#include "Ipc.h"
#include <ntstrsafe.h>

void SofLogBreadcrumb(const char* text);
NTSYSAPI NTSTATUS NTAPI ZwFlushBuffersFile(_In_ HANDLE FileHandle, _Out_ PIO_STATUS_BLOCK IoStatusBlock);

/**
 * @def SOF_IPC_MAX_PAYLOAD
 * @brief Maximum supported IPC payload buffer size in bytes (4096 = 4KB page).
 */
#define SOF_IPC_MAX_PAYLOAD 4096

/**
 * @brief Initializes IPC synchronization primitives and allocates the shared reply buffer.
 *
 * Prepares the host-side IPC infrastructure by initializing the spinlock protecting
 * transmit state, resetting communication events, allocating non-paged memory for
 * receiving DSP replies, and setting the firmware boot state to NOT_STARTED.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return STATUS_SUCCESS on success, or STATUS_INSUFFICIENT_RESOURCES if pool allocation fails.
 */
NTSTATUS
SofIpcInitialize(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    /* Initialize spinlock protecting IPC transmitter state and pending reply buffer */
    KeInitializeSpinLock(&Ctx->IpcLock);

    /* Notification events: manually cleared before dispatch, signaled by DPC on completion */
    KeInitializeEvent(&Ctx->IpcReplyEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Ctx->FwReadyEvent, NotificationEvent, FALSE);

    /* Allocate non-paged kernel pool for holding incoming mailbox reply payloads */
    Ctx->ReplyBufferSize = SOF_IPC_MAX_PAYLOAD;
    Ctx->ReplyBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                       Ctx->ReplyBufferSize,
                                       SOF_DEVICE_TAG);
    if (!Ctx->ReplyBuffer) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Failed to allocate IPC reply buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Ctx->ReplyBuffer, Ctx->ReplyBufferSize);
    Ctx->IpcTxBusy = FALSE;
    Ctx->FwState = SOF_FW_BOOT_NOT_STARTED;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC subsystem initialized successfully.\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Releases IPC subsystem resources during driver unload or tear-down.
 *
 * Frees the allocated non-paged reply buffer and zeros out tracking fields.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcCleanup(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (Ctx->ReplyBuffer) {
        ExFreePoolWithTag(Ctx->ReplyBuffer, SOF_DEVICE_TAG);
        Ctx->ReplyBuffer = NULL;
        Ctx->ReplyBufferSize = 0;
    }
}

/**
 * @brief Enables host-directed IPC interrupts in the DSP hardware registers.
 *
 * Programs the multi-level interrupt hierarchy on Intel hardware:
 *  1. Top-Level: Reads MTL_HFINTIPPTR to find the IRQINTEN register offset and enables HOST_IPC.
 *  2. Second-Level: Sets MTL_DSP_REG_HfHIPCIE_IE_MASK in HfHIPCIE.
 *  3. Channel-Level: Unmasks BUSY and DONE interrupts in HIPCCTL.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcEnableInterrupts(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return;

    if (Ctx->HwDesc->IsAce) {
        /*
         * ACE Interrupt Architecture Level 1:
         * Read Interrupt IP Pointer (MTL_HFINTIPPTR) to determine the base offset
         * of the top-level interrupt controller block within DSP BAR4 MMIO space.
         */
        ULONG hfintipptr = SofDspRead32(Ctx->Bar4VirtualAddress, MTL_HFINTIPPTR) & MTL_HFINTIPPTR_PTR_MASK;

        /*
         * ACE Interrupt Architecture Level 1 Enable:
         * Unmask Host IPC interrupt generation in the primary IRQINTEN register.
         */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         hfintipptr,
                         MTL_IRQ_INTEN_L_HOST_IPC_MASK,
                         MTL_IRQ_INTEN_L_HOST_IPC_MASK);

        /*
         * ACE Interrupt Architecture Level 2 Enable:
         * Unmask the host-facing IPC interrupt in the intermediate HfHIPCIE register.
         */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         MTL_DSP_REG_HfHIPCIE,
                         MTL_DSP_REG_HfHIPCIE_IE_MASK,
                         MTL_DSP_REG_HfHIPCIE_IE_MASK);
    }

    /*
     * Channel-Level IPC Interrupt Enable (cAVS and ACE):
     * Program HIPCCTL to enable interrupt generation on:
     *  - BUSY: DSP raised an incoming message or notification in HIPCTDR.
     *  - DONE: DSP acknowledged host-transmitted command in HIPCIDA.
     */
    SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                     Ctx->HwDesc->HipcCtl,
                     Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone,
                     Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC interrupts enabled (IsAce=%d, CTL offset 0x%x).\n",
               Ctx->HwDesc->IsAce, Ctx->HwDesc->HipcCtl);
}

/**
 * @brief Disables host-directed IPC interrupts in the DSP hardware registers.
 *
 * Masks BUSY and DONE interrupts in HIPCCTL, and disables intermediate and top-level
 * interrupt registers on ACE platforms to guarantee quiet hardware before power transition.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcDisableInterrupts(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return;

    /*
     * Channel-Level IPC Interrupt Disable:
     * Mask both BUSY and DONE interrupts in the channel control register (HIPCCTL).
     */
    SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                     Ctx->HwDesc->HipcCtl,
                     Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone,
                     0);

    if (Ctx->HwDesc->IsAce) {
        /* Read Interrupt IP Pointer to locate top-level IRQ block */
        ULONG hfintipptr = SofDspRead32(Ctx->Bar4VirtualAddress, MTL_HFINTIPPTR) & MTL_HFINTIPPTR_PTR_MASK;

        /* Disable Host IPC interrupt in second-level HfHIPCIE */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         MTL_DSP_REG_HfHIPCIE,
                         MTL_DSP_REG_HfHIPCIE_IE_MASK,
                         0);

        /* Disable Host IPC in top-level IRQINTEN */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         hfintipptr,
                         MTL_IRQ_INTEN_L_HOST_IPC_MASK,
                         0);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC interrupts disabled.\n");
}

/**
 * @brief Captures and persists full DSP state, registers, mailboxes, and exception frames.
 *
 * Executed upon firmware crash notification (SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT), transmission
 * timeout, or fatal hardware error. Captures:
 *  - DSP Core 0 Status (HFDSC / 0x160200) and Error (HFDEC / 0x160204).
 *  - Extended ROM status registers [0..7].
 *  - All IPC doorbell registers (HIPCTDR, HIPCTDA, HIPCTDD, HIPCIDR, HIPCIDA, HIPCIDD, HIPCCTL).
 *  - Full 4KB Uplink and 4KB Downlink mailbox memory blocks.
 *  - Xtensa architecture exception frame (EXCCAUSE, EXCVADDR, EPC1, PS) if present.
 * Writes diagnostic files to:
 *  - C:\\sof_dsp_coredump.bin (raw binary for offline post-mortem analysis).
 *  - C:\\sof_dsp_coredump.txt (human-readable structured diagnostic report).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @param[in] Reason Diagnostic string identifying the failure trigger.
 */
VOID
SofDspDumpCore(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ const char *Reason
)
{
    LARGE_INTEGER sysTime;
    ULONG core0Status;
    ULONG core0Error;
    ULONG extRom[8] = { 0 };
    ULONG hipcTdr, hipcTda, hipcTdd, hipcIdr, hipcIda, hipcIdd, hipcCtl;
    ULONG dumpSize;
    PUCHAR dumpBuffer;
    PSOF_DSP_COREDUMP_HEADER hdr;
    PUCHAR uplinkDst;
    PUCHAR downlinkDst;
    PULONG upDwords;
    BOOLEAN hasOops = FALSE;
    ULONG exccause = 0, excvaddr = 0, epc1 = 0, ps = 0;

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc) {
        return;
    }

    KeQuerySystemTime(&sysTime);

    /* Read Core 0 hardware status and error registers from DSP SHIM */
    core0Status = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomStatusReg);
    core0Error = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomErrorReg);

    /* Read extended ROM status array (8 consecutive DWORDs) */
    for (ULONG i = 0; i < 8; i++) {
        extRom[i] = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomStatusReg + i * 4);
    }

    /* Read all IPC doorbell and control registers */
    hipcTdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr);
    hipcTda = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda);
    hipcTdd = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdd);
    hipcIdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIdr);
    hipcIda = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIda);
    hipcIdd = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIdd);
    hipcCtl = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcCtl);

    /* Calculate total dump buffer size: Header + 4KB Uplink Mailbox + 4KB Downlink Mailbox + 64KB Debug Window */
    ULONG debugWinOffset = Ctx->DebugWindowOffset;
    if (debugWinOffset == 0 && Ctx->HwDesc) {
        debugWinOffset = Ctx->HwDesc->IsAce ? 0x190000 : 0xC0000;
        Ctx->DebugWindowOffset = debugWinOffset;
    }
    ULONG debugWinSize = 65536; /* 16 pages of 4KB */

    dumpSize = sizeof(SOF_DSP_COREDUMP_HEADER) + 4096 + 4096 + debugWinSize;
    dumpBuffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, dumpSize, SOF_DEVICE_TAG);
    if (!dumpBuffer) {
        SofLogBreadcrumb("[DSP-PANIC] Failed to allocate coredump buffer\n");
        return;
    }

    /* Populate structured core dump header */
    RtlZeroMemory(dumpBuffer, dumpSize);
    hdr = (PSOF_DSP_COREDUMP_HEADER)dumpBuffer;
    hdr->Magic = SOF_COREDUMP_MAGIC;
    hdr->Version = SOF_COREDUMP_VERSION;
    hdr->DumpSize = dumpSize;
    hdr->TimestampLow = sysTime.LowPart;
    hdr->TimestampHigh = sysTime.HighPart;
    hdr->VendorId = 0x8086;
    hdr->DeviceId = 0xE428;
    if (Reason) {
        RtlStringCbCopyA(hdr->ReasonString, sizeof(hdr->ReasonString), Reason);
    }
    hdr->Core0Status = core0Status;
    hdr->Core0Error = core0Error;
    for (ULONG i = 0; i < 8; i++) {
        hdr->ExtRomStatus[i] = extRom[i];
    }
    hdr->HipcTdr = hipcTdr;
    hdr->HipcTda = hipcTda;
    hdr->HipcTdd = hipcTdd;
    hdr->HipcIdr = hipcIdr;
    hdr->HipcIda = hipcIda;
    hdr->HipcIdd = hipcIdd;
    hdr->HipcCtl = hipcCtl;
    hdr->UplinkMailboxOffset = Ctx->HwDesc->MailboxUplinkOffset;
    hdr->UplinkMailboxSize = 4096;
    hdr->DownlinkMailboxOffset = Ctx->HwDesc->MailboxDownlinkOffset;
    hdr->DownlinkMailboxSize = 4096;
    hdr->DebugWindowOffset = debugWinOffset;
    hdr->DebugWindowSize = debugWinSize;

    uplinkDst = dumpBuffer + sizeof(SOF_DSP_COREDUMP_HEADER);
    downlinkDst = uplinkDst + 4096;
    PUCHAR debugWinDst = downlinkDst + 4096;

    /*
     * Read full 4KB Uplink Mailbox (DSP SRAM) via 32-bit MMIO reads.
     * Contains firmware crash log, exception headers, and active pipeline context.
     */
    if (Ctx->HwDesc->MailboxUplinkOffset > 0) {
        volatile ULONG *src = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + Ctx->HwDesc->MailboxUplinkOffset);
        ULONG *dst = (ULONG *)uplinkDst;
        for (ULONG i = 0; i < 4096 / sizeof(ULONG); i++) {
            dst[i] = src[i];
        }
    }

    /*
     * Read full 4KB Downlink Mailbox (DSP SRAM) via 32-bit MMIO reads.
     * Contains the last command payload dispatched from host to DSP.
     */
    if (Ctx->HwDesc->MailboxDownlinkOffset > 0) {
        volatile ULONG *src = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + Ctx->HwDesc->MailboxDownlinkOffset);
        ULONG *dst = (ULONG *)downlinkDst;
        for (ULONG i = 0; i < 4096 / sizeof(ULONG); i++) {
            dst[i] = src[i];
        }
    }

    /*
     * Read 64KB SRAM Debug Window (Page 0 descriptors + Core slots) via 32-bit MMIO reads.
     * Captures Zephyr Mtrace circular buffers for offline post-mortem log reconstruction.
     */
    if (debugWinOffset > 0 && debugWinOffset + debugWinSize <= Ctx->Bar4Length) {
        volatile ULONG *src = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + debugWinOffset);
        ULONG *dst = (ULONG *)debugWinDst;
        for (ULONG i = 0; i < debugWinSize / sizeof(ULONG); i++) {
            dst[i] = src[i];
        }
    }

    /*
     * Parse Xtensa Architecture Exception / Oops Frame:
     * If the Uplink mailbox begins with type=1 and stack frame size >= 28,
     * extract hardware exception registers: EXCCAUSE, EXCVADDR, PS, EPC1.
     */
    upDwords = (PULONG)uplinkDst;
    if (upDwords[0] == 1 && upDwords[1] >= 28) {
        hasOops = TRUE;
        exccause = upDwords[7];
        excvaddr = upDwords[8];
        ps = upDwords[9];
        epc1 = upDwords[10];
    }

    /* Format Breadcrumb / Debugger Output */
    {
        char msg[256];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[DSP-PANIC] *** DSP EXCEPTION DUMP: Reason=%s ***\n",
            Reason ? Reason : "UNKNOWN");
        SofLogBreadcrumb(msg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[DSP-PANIC] Core0Status=0x%08X Core0Error=0x%08X TDR=0x%08X IDR=0x%08X\n",
            core0Status, core0Error, hipcTdr, hipcIdr);
        SofLogBreadcrumb(msg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[DSP-PANIC] ExtROM: [0..3]=0x%08X 0x%08X 0x%08X 0x%08X [4..7]=0x%08X 0x%08X 0x%08X 0x%08X\n",
            extRom[0], extRom[1], extRom[2], extRom[3],
            extRom[4], extRom[5], extRom[6], extRom[7]);
        SofLogBreadcrumb(msg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);

        if (hasOops) {
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[DSP-PANIC] Xtensa Oops: EXCCAUSE=0x%X EXCVADDR=0x%08X EPC1=0x%08X PS=0x%08X\n",
                exccause, excvaddr, epc1, ps);
            SofLogBreadcrumb(msg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);
        }
    }

    /*
     * Persist Raw Binary Dump File:
     * Written to \DosDevices\C:\sof_dsp_coredump.bin for deep analysis by tools like coredumper.
     */
    {
        UNICODE_STRING binPath;
        OBJECT_ATTRIBUTES binOa;
        IO_STATUS_BLOCK binIo;
        HANDLE hBin = NULL;

        RtlInitUnicodeString(&binPath, L"\\DosDevices\\C:\\sof_dsp_coredump.bin");
        InitializeObjectAttributes(&binOa, &binPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

        if (NT_SUCCESS(ZwCreateFile(&hBin, GENERIC_WRITE | SYNCHRONIZE, &binOa, &binIo, NULL,
                                     FILE_ATTRIBUTE_NORMAL,
                                     FILE_SHARE_READ,
                                     FILE_OVERWRITE_IF,
                                     FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            ZwWriteFile(hBin, NULL, NULL, NULL, &binIo, dumpBuffer, dumpSize, NULL, NULL);
            ZwFlushBuffersFile(hBin, &binIo);
            ZwClose(hBin);
            SofLogBreadcrumb("[DSP-PANIC] Binary core dump written to C:\\sof_dsp_coredump.bin\n");
        } else {
            SofLogBreadcrumb("[DSP-PANIC] Failed to write C:\\sof_dsp_coredump.bin\n");
        }
    }

    /*
     * Persist Human-Readable Text Report:
     * Written to \DosDevices\C:\sof_dsp_coredump.txt for instant field diagnostics without symbol setup.
     */
    {
        UNICODE_STRING txtPath;
        OBJECT_ATTRIBUTES txtOa;
        IO_STATUS_BLOCK txtIo;
        HANDLE hTxt = NULL;

        RtlInitUnicodeString(&txtPath, L"\\DosDevices\\C:\\sof_dsp_coredump.txt");
        InitializeObjectAttributes(&txtOa, &txtPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

        if (NT_SUCCESS(ZwCreateFile(&hTxt, GENERIC_WRITE | SYNCHRONIZE, &txtOa, &txtIo, NULL,
                                     FILE_ATTRIBUTE_NORMAL,
                                     FILE_SHARE_READ,
                                     FILE_OVERWRITE_IF,
                                     FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            char txtBuf[2048];
            PULONG downDwords = (PULONG)downlinkDst;
            RtlStringCbPrintfA(txtBuf, sizeof(txtBuf),
                "================================================================================\r\n"
                "SOUND OPEN FIRMWARE (SOF) DSP EXCEPTION / CORE DUMP REPORT\r\n"
                "================================================================================\r\n"
                "Reason: %s\r\n"
                "Timestamp: 0x%08X%08X\r\n"
                "Hardware: Vendor=0x%04X, Device=0x%04X (Panther Lake)\r\n"
                "\r\n"
                "DSP Core0 Status (HFDSC @ 0x160200): 0x%08X\r\n"
                "DSP Core0 Error  (HFDEC @ 0x160204): 0x%08X\r\n"
                "Extended ROM Status [0..7]:\r\n"
                "  [0]: 0x%08X  [1]: 0x%08X  [2]: 0x%08X  [3]: 0x%08X\r\n"
                "  [4]: 0x%08X  [5]: 0x%08X  [6]: 0x%08X  [7]: 0x%08X\r\n"
                "\r\n"
                "IPC Channel Registers:\r\n"
                "  HIPCTDR: 0x%08X (Busy=%u)\r\n"
                "  HIPCTDA: 0x%08X (Done=%u)\r\n"
                "  HIPCTDD: 0x%08X\r\n"
                "  HIPCIDR: 0x%08X (Busy=%u)\r\n"
                "  HIPCIDA: 0x%08X (Done=%u)\r\n"
                "  HIPCIDD: 0x%08X\r\n"
                "  HIPCCTL: 0x%08X\r\n"
                "\r\n"
                "Xtensa Oops / Exception Frame Analysis:\r\n"
                "  Oops Detected: %s\r\n"
                "  EXCCAUSE:      0x%08X\r\n"
                "  EXCVADDR:      0x%08X\r\n"
                "  EPC1:          0x%08X\r\n"
                "  PS:            0x%08X\r\n"
                "\r\n"
                "Uplink Mailbox First 32 Bytes (Hex):\r\n"
                "  %08X %08X %08X %08X %08X %08X %08X %08X\r\n"
                "Downlink Mailbox First 32 Bytes (Hex):\r\n"
                "  %08X %08X %08X %08X %08X %08X %08X %08X\r\n"
                "================================================================================\r\n",
                Reason ? Reason : "UNKNOWN",
                hdr->TimestampHigh, hdr->TimestampLow,
                hdr->VendorId, hdr->DeviceId,
                core0Status, core0Error,
                extRom[0], extRom[1], extRom[2], extRom[3],
                extRom[4], extRom[5], extRom[6], extRom[7],
                hipcTdr, (hipcTdr & Ctx->HwDesc->HipcTdrBusy) ? 1 : 0,
                hipcTda, (hipcTda & Ctx->HwDesc->HipcTdaDone) ? 1 : 0,
                hipcTdd,
                hipcIdr, (hipcIdr & Ctx->HwDesc->HipcIdrBusy) ? 1 : 0,
                hipcIda, (hipcIda & Ctx->HwDesc->HipcIdaDone) ? 1 : 0,
                hipcIdd, hipcCtl,
                hasOops ? "YES" : "NO",
                exccause, excvaddr, epc1, ps,
                upDwords[0], upDwords[1], upDwords[2], upDwords[3],
                upDwords[4], upDwords[5], upDwords[6], upDwords[7],
                downDwords[0], downDwords[1], downDwords[2], downDwords[3],
                downDwords[4], downDwords[5], downDwords[6], downDwords[7]);

            size_t txtLen = 0;
            RtlStringCbLengthA(txtBuf, sizeof(txtBuf), &txtLen);
            ZwWriteFile(hTxt, NULL, NULL, NULL, &txtIo, txtBuf, (ULONG)txtLen, NULL, NULL);
            ZwFlushBuffersFile(hTxt, &txtIo);
            ZwClose(hTxt);
            SofLogBreadcrumb("[DSP-PANIC] Formatted text report written to C:\\sof_dsp_coredump.txt\n");
        } else {
            SofLogBreadcrumb("[DSP-PANIC] Failed to write C:\\sof_dsp_coredump.txt\n");
        }
    }

    ExFreePoolWithTag(dumpBuffer, SOF_DEVICE_TAG);
}

/**
 * @brief Fast Interrupt Service Routine (ISR) check for pending IPC hardware events.
 *
 * Checks top-level and channel-level doorbell registers:
 *  - On ACE, verifies top-level IRQ status (MTL_DSP_IRQSTS_IPC).
 *  - Checks if DSP raised an incoming message (HIPCTDR BUSY asserted).
 *  - Checks if DSP completed a host command (HIPCIDA DONE asserted).
 * If either condition is met, masks HIPCCTL BUSY and DONE interrupts immediately to
 * eliminate line-based IRQ storms, then returns TRUE to schedule the DPC.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return TRUE if an IPC interrupt was acknowledged and DPC is needed; FALSE otherwise.
 */
BOOLEAN
SofIpcIsrCheck(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    ULONG tdr;
    ULONG ida;

    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return FALSE;

    /* For ACE platforms (MTL, LNL, PTL), verify top-level DSP IRQ status register */
    if (Ctx->HwDesc->IsAce && Ctx->HwDesc->IntIpPtrReg) {
        ULONG ptr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->IntIpPtrReg) & MTL_HFINTIPPTR_PTR_MASK;
        ULONG irqSts = SofDspRead32(Ctx->Bar4VirtualAddress, ptr + MTL_DSP_IRQSTS);

        /* Return FALSE immediately if register is unmapped (0xFFFFFFFF) or IPC bit is not set */
        if (irqSts == 0xFFFFFFFF || !(irqSts & MTL_DSP_IRQSTS_IPC)) {
            return FALSE;
        }
    }

    /* Check whether DSP raised an incoming message (TDR BUSY) or acknowledged our command (IDA DONE) */
    tdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr);
    ida = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIda);

    if ((tdr & Ctx->HwDesc->HipcTdrBusy) || (ida & Ctx->HwDesc->HipcIdaDone)) {
        /*
         * CRITICAL: Mask IPC interrupts immediately in ISR to avoid an interrupt storm
         * on shared line-based interrupts before the DPC can run.
         */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone,
                         0);
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief Deferred Procedure Call (DPC) worker for dispatching IPC replies and notifications.
 *
 * Evaluates pending hardware status:
 *  1. Host TX ACK: DSP set DONE in HIPCIDA. Acknowledges DONE in HIPCIDA, re-enables HIPCCTL.
 *  2. DSP Incoming Message: DSP set BUSY in HIPCTDR.
 *     - If message direction bit (SOF_IPC4_MSG_DIR_MASK) is 1, this is a command reply.
 *       Reads payload from Uplink Mailbox, unlocks transmitter, and signals IpcReplyEvent.
 *     - If message direction bit is 0, this is an unsolicited notification.
 *       Handles SOF_IPC4_NOTIFY_FW_READY (signals FwReadyEvent) or
 *       SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT (triggers SofDspDumpCore).
 *  3. Doorbell Acknowledgment: Clears BUSY in HIPCTDR and HIPCTDA to acknowledge receipt to DSP.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcDpcHandler(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    ULONG ida;
    ULONG tdr;

    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return;

    /*
     * 1. Check if DSP completed our previous host-initiated request (ACK / DONE in IDA)
     */
    ida = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIda);
    if (ida & Ctx->HwDesc->HipcIdaDone) {
        /* Mask DONE in HipcCtl */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlDone,
                         0);

        if (Ctx->HwDesc->IsAce) {
            /* Set DONE bit in IDA to acknowledge receipt to DSP on ACE */
            SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                             Ctx->HwDesc->HipcIda,
                             Ctx->HwDesc->HipcIdaDone,
                             Ctx->HwDesc->HipcIdaDone);
        } else {
            /* Write DONE bit to IDA on cAVS */
            SofDspWrite32(Ctx->Bar4VirtualAddress,
                          Ctx->HwDesc->HipcIda,
                          Ctx->HwDesc->HipcIdaDone);
        }

        /* Unmask DONE in HipcCtl */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlDone,
                         Ctx->HwDesc->HipcCtlDone);

        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] IPC Host TX ACK received from DSP (IDA=0x%08x).\n",
                   ida);
    }

    /*
     * 2. Check if DSP sent an incoming message (Reply or Notification in TDR)
     */
    tdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr);
    if (tdr & Ctx->HwDesc->HipcTdrBusy) {
        ULONG tdd = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdd);
        ULONG primary = tdr & 0x7FFFFFFF;
        ULONG extension = tdd & 0x7FFFFFFF;

        /* Is this a reply to our request or an unsolicited notification? */
        if (primary & SOF_IPC4_MSG_DIR_MASK) {
            /*
             * Reply Message: Matches outstanding host request.
             */
            KIRQL oldIrql;
            KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);

            Ctx->PendingReply.Primary = primary;
            Ctx->PendingReply.Extension = extension;

            /* If there is payload in the uplink mailbox, read it via 32-bit DWORD MMIO reads */
            if (Ctx->ReplyBuffer && Ctx->HwDesc->MailboxUplinkSize > 0) {
                volatile ULONG *src = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + Ctx->HwDesc->MailboxUplinkOffset);
                ULONG *dst = (ULONG *)Ctx->ReplyBuffer;
                ULONG copyDwords = min(Ctx->ReplyBufferSize, Ctx->HwDesc->MailboxUplinkSize) / sizeof(ULONG);
                for (ULONG i = 0; i < copyDwords; i++) {
                    dst[i] = src[i];
                }
                Ctx->PendingReply.Payload = Ctx->ReplyBuffer;
                Ctx->PendingReply.PayloadSize = min(Ctx->ReplyBufferSize, Ctx->HwDesc->MailboxUplinkSize);
            }

            Ctx->IpcTxBusy = FALSE;
            KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
            KeSetEvent(&Ctx->IpcReplyEvent, IO_NO_INCREMENT, FALSE);

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] IPC Reply: Primary 0x%08x, Ext 0x%08x (status=0x%x)\n",
                       primary, extension, primary & SOF_IPC4_REPLY_STATUS);
        } else {
            /*
             * Unsolicited Notification: Sent asynchronously by DSP firmware.
             */
            ULONG notifyType = SOF_IPC4_NOTIFICATION_TYPE_GET(primary);

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] IPC Notification: Type %u (0x%08x|0x%08x)\n",
                       notifyType, primary, extension);

            if (notifyType == SOF_IPC4_NOTIFY_FW_READY) {
                /* Firmware initialization complete and scheduler running */
                char bmsg[128];
                RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                    "[13] *** DSP FIRMWARE BOOT COMPLETED (FW_READY: 0x%08x|0x%08x) ***\n",
                    primary, extension);
                SofLogBreadcrumb(bmsg);
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", bmsg);
                Ctx->FwState = SOF_FW_BOOT_COMPLETE;
                KeSetEvent(&Ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
            } else if (notifyType == SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT) {
                /* Firmware panic/exception caught by base FW */
                char bmsg[128];
                RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                    "[DSP-PANIC] *** FIRMWARE EXCEPTION NOTIFICATION RECEIVED (0x%08x|0x%08x) ***\n",
                    primary, extension);
                SofLogBreadcrumb(bmsg);
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", bmsg);
                SofDspDumpCore(Ctx, "IPC4_NOTIFY_EXCEPTION_CAUGHT");
            } else if (notifyType == SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS) {
                /*
                 * Firmware Log Buffer Status: DSP has generated new log data
                 * in its SRAM debug slot (CONFIG_LOG_BACKEND_ADSP_MTRACE).
                 */
                ULONG core = SOF_IPC4_LOG_CORE_GET(primary);
                SofIpc4DrainFwLogs(Ctx, core);
            }
        }

        /*
         * 3. Acknowledge message receipt to DSP:
         * On ACE (mtl_ipc_host_done):
         *  - Step a: Set busy bit in TDR to clear the interrupt assertion.
         *  - Step b: Clear busy bit in TDA to signal completion back to DSP.
         * On cAVS:
         *  - Write busy bit to TDR and done bit to TDA.
         */
        if (Ctx->HwDesc->IsAce) {
            SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                             Ctx->HwDesc->HipcTdr,
                             Ctx->HwDesc->HipcTdrBusy,
                             Ctx->HwDesc->HipcTdrBusy);

            SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                             Ctx->HwDesc->HipcTda,
                             Ctx->HwDesc->HipcTdaDone,
                             0);
        } else {
            SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr, Ctx->HwDesc->HipcTdrBusy);
            SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda, Ctx->HwDesc->HipcTdaDone);
        }

        /* Unmask Busy interrupt in HipcCtl */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlBusy,
                         Ctx->HwDesc->HipcCtlBusy);
    } else {
        /* Ensure BUSY interrupt remains unmasked */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlBusy,
                         Ctx->HwDesc->HipcCtlBusy);
    }
}

/**
 * @brief Synchronously transmits an IPC4 message and awaits the DSP reply.
 *
 * Transmit Sequence:
 *  1. Acquires IpcLock spinlock and checks transmitter busy state.
 *  2. Writes optional payload data into the Downlink Mailbox (DSP SRAM) using 32-bit MMIO writes.
 *  3. Writes Extension header to HIPCIDD.
 *  4. Rings the Doorbell: Writes Primary header with BUSY bit asserted to HIPCIDR.
 *  5. Releases spinlock and enters a wait loop on IpcReplyEvent.
 *  6. Polling Fallback: Checks every 25ms and manually calls SofIpcDpcHandler in case a line-based
 *     interrupt was delayed or lost under heavy system load.
 *  7. On completion, validates the IPC4 reply status code (bits 0..23). On timeout, triggers SofDspDumpCore.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @param[in]     Msg Pointer to the IPC message struct to send.
 * @param[out]    Reply Optional pointer to receive reply headers and payload.
 * @param[in]     TimeoutMs Maximum duration to await response in milliseconds.
 * @return STATUS_SUCCESS on success; STATUS_DEVICE_BUSY if already sending; STATUS_IO_TIMEOUT
 *         on timeout; STATUS_UNSUCCESSFUL if DSP returned an error code.
 */
NTSTATUS
SofIpcSendMessage(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PSOF_IPC_MESSAGE Msg,
    _Out_opt_ PSOF_IPC_MESSAGE Reply,
    _In_ ULONG TimeoutMs
)
{
    KIRQL oldIrql;
    LARGE_INTEGER timeout;
    NTSTATUS status = STATUS_IO_TIMEOUT;
    ULONG elapsedMs = 0;

    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return STATUS_DEVICE_NOT_READY;

    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);

    if (Ctx->IpcTxBusy) {
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        return STATUS_DEVICE_BUSY;
    }

    Ctx->IpcTxBusy = TRUE;
    KeResetEvent(&Ctx->IpcReplyEvent);

    /*
     * Write payload to Mailbox Downlink if present using 32-bit DWORD MMIO writes.
     * Host writes must align to DWORD boundaries for DSP SRAM controller consistency.
     */
    if (Msg->Payload && Msg->PayloadSize > 0) {
        ULONG writeSize = min(Msg->PayloadSize, Ctx->HwDesc->MailboxDownlinkSize);
        volatile ULONG *dst = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + Ctx->HwDesc->MailboxDownlinkOffset);
        const ULONG *src = (const ULONG *)Msg->Payload;
        ULONG dwordCount = (writeSize + 3) / sizeof(ULONG);
        for (ULONG i = 0; i < dwordCount; i++) {
            dst[i] = src[i];
        }
    }

    /* Write extension header to HIPCIDD */
    SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIdd, Msg->Extension);

    /*
     * Ring Doorbell:
     * Write Primary header with BUSY bit asserted to HIPCIDR.
     * This signals the DSP hardware scheduler that a host command is ready for processing.
     */
    SofDspWrite32(Ctx->Bar4VirtualAddress,
                  Ctx->HwDesc->HipcIdr,
                  Msg->Primary | Ctx->HwDesc->HipcIdrBusy);

    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);

    {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[IPC-TX] Pri=0x%08x Ext=0x%08x Sz=%u\n",
            Msg->Primary, Msg->Extension, Msg->PayloadSize);
        SofLogBreadcrumb(bmsg);
    }
    if (Msg->Payload && Msg->PayloadSize >= 16) {
        PULONG p = (PULONG)Msg->Payload;
        char dmsg[160];
        RtlStringCbPrintfA(dmsg, sizeof(dmsg),
            "[IPC-D] [0..5]=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
            p[0], p[1], p[2], p[3], p[4], p[5]);
        SofLogBreadcrumb(dmsg);

        /* Read back from hardware mailbox to confirm MMIO write completion */
        volatile ULONG *rb = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + Ctx->HwDesc->MailboxDownlinkOffset);
        RtlStringCbPrintfA(dmsg, sizeof(dmsg),
            "[IPC-RB] [0..5]=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
            rb[0], rb[1], rb[2], rb[3], rb[4], rb[5]);
        SofLogBreadcrumb(dmsg);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC TX Sent: Primary 0x%08x, Ext 0x%08x\n",
               Msg->Primary, Msg->Extension);

    /*
     * Synchronous Wait with Polling Fallback:
     * Waits in 25ms increments. If an interrupt was delayed or missed by the kernel
     * under heavy load, calls SofIpcDpcHandler directly to check hardware status.
     */
    while (elapsedMs < TimeoutMs) {
        timeout.QuadPart = -10000LL * 25; /* 25ms */
        status = KeWaitForSingleObject(&Ctx->IpcReplyEvent,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       &timeout);
        if (status == STATUS_SUCCESS) {
            break;
        }

        /* Poll hardware registers directly */
        SofIpcDpcHandler(Ctx);
        if (KeReadStateEvent(&Ctx->IpcReplyEvent) != 0) {
            status = STATUS_SUCCESS;
            break;
        }

        elapsedMs += 25;
    }

    if (status != STATUS_SUCCESS) {
        KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
        Ctx->IpcTxBusy = FALSE; /* Reset busy flag so subsequent commands are not blocked */
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);

        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] IPC TX Timed out waiting for DSP reply (%u ms, primary=0x%08x)!\n",
                   TimeoutMs, Msg->Primary);
        SofDspDumpCore(Ctx, "IPC_TRANSMIT_TIMEOUT");
        return STATUS_IO_TIMEOUT;
    }

    /* Copy pending reply data into caller's structure if requested */
    if (Reply) {
        KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
        Reply->Primary = Ctx->PendingReply.Primary;
        Reply->Extension = Ctx->PendingReply.Extension;
        Reply->PayloadSize = Ctx->PendingReply.PayloadSize;
        if (Reply->Payload && Ctx->PendingReply.Payload) {
            RtlCopyMemory(Reply->Payload,
                          Ctx->PendingReply.Payload,
                          min(Reply->PayloadSize, Ctx->PendingReply.PayloadSize));
        }
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
    }

    /* Check status field in IPC4 reply header (bits 0..23; 0 indicates success) */
    {
        ULONG replyStatus = Ctx->PendingReply.Primary & SOF_IPC4_REPLY_STATUS;
        if (replyStatus != 0) {
            char bmsg[160];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[IPC-ERR] Pri=0x%08x Ext=0x%08x RepPri=0x%08x RepExt=0x%08x (status=0x%x)\n",
                Msg->Primary, Msg->Extension, Ctx->PendingReply.Primary, Ctx->PendingReply.Extension, replyStatus);
            SofLogBreadcrumb(bmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, "%s", bmsg);
            return STATUS_UNSUCCESSFUL;
        } else {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[IPC-OK] Primary=0x%08x Reply=0x%08x\n",
                Msg->Primary, Ctx->PendingReply.Primary);
            SofLogBreadcrumb(bmsg);
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Reads the 32-bit DSP Firmware Status register (FWSTATUS / HFDSC).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @return 32-bit status value, or 0 if unmapped.
 */
ULONG
SofReadFwStatus(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return 0;

    return SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->FwStatusReg);
}

/**
 * @brief Reads the 32-bit DSP ROM Status register (ROMSTATUS / HFDSC).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @return 32-bit status value, or 0 if unmapped.
 */
ULONG
SofReadRomStatus(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return 0;

    return SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomStatusReg);
}

/**
 * @brief Reads unread Zephyr firmware logs from the DSP SRAM Debug Window.
 *
 * Implements the host side of the Zephyr ADSP Mtrace protocol (CONFIG_LOG_BACKEND_ADSP_MTRACE).
 * Invoked on SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS notification from DSP.
 *
 * Steps:
 *  1. Identifies debug slot offset for the given DSP core from Page 0 descriptors.
 *  2. Reads DSP write pointer (dsp_write_ptr) from the slot header.
 *  3. Reads newly arrived log bytes between host_read_ptr and dsp_write_ptr.
 *  4. Forwards log lines to debugger or breadcrumb log buffer.
 *  5. Writes updated host_read_ptr back to the slot header in DSP SRAM.
 *
 * @param[in,out] Ctx  Pointer to the SOF device context.
 * @param[in]     Core Zero-based DSP core index (0..3).
 */
VOID
SofIpc4DrainFwLogs(
    _Inout_ PSOF_DEVICE_CONTEXT Ctx,
    _In_    ULONG               Core
)
{
    ULONG debugWinOffset;
    ULONG slotOffset;
    ULONG dspWritePtr, hostReadPtr, avail, logBufSize;
    PUCHAR sramBase;

    if (!Ctx || !Ctx->Bar4VirtualAddress || Core >= 4) {
        return;
    }

    sramBase = (PUCHAR)Ctx->Bar4VirtualAddress;
    debugWinOffset = Ctx->DebugWindowOffset;
    if (debugWinOffset == 0) {
        debugWinOffset = (Ctx->HwDesc && Ctx->HwDesc->IsAce) ? 0x190000 : 0xC0000;
        Ctx->DebugWindowOffset = debugWinOffset;
    }

    /* Locate slot offset for this core if not yet cached */
    slotOffset = Ctx->FwLogSlotOffset[Core];
    if (slotOffset == 0) {
        for (ULONG i = 0; i < 15; i++) {
            ULONG descTypeOffset = debugWinOffset + i * 12 + 4;
            ULONG type = SofDspRead32(sramBase, descTypeOffset);
            if ((type & 0xFFFFFF00) == 0x474F4C00 && (type & 0xFF) == Core) {
                slotOffset = debugWinOffset + (i + 1) * 4096;
                Ctx->FwLogSlotOffset[Core] = slotOffset;
                break;
            }
        }
        if (slotOffset == 0) {
            slotOffset = debugWinOffset + (Core + 1) * 4096;
            Ctx->FwLogSlotOffset[Core] = slotOffset;
        }
    }

    /* Read DSP write pointer and host read pointer */
    hostReadPtr = SofDspRead32(sramBase, slotOffset);
    dspWritePtr = SofDspRead32(sramBase, slotOffset + 4);
    logBufSize = 4096 - 8; /* 4088 bytes circular buffer */

    if (dspWritePtr >= logBufSize) dspWritePtr = 0;
    if (hostReadPtr >= logBufSize) hostReadPtr = 0;

    if (dspWritePtr == hostReadPtr) {
        return;
    }

    avail = (dspWritePtr > hostReadPtr) ?
            (dspWritePtr - hostReadPtr) :
            (logBufSize - hostReadPtr + dspWritePtr);

    if (avail > 0 && avail <= logBufSize) {
        CHAR lineBuf[256];
        ULONG bytesToRead = (avail > 240) ? 240 : avail;

        for (ULONG b = 0; b < bytesToRead; b++) {
            ULONG bytePos = (hostReadPtr + b) % logBufSize;
            lineBuf[b] = (CHAR)SofDspRead8(sramBase, slotOffset + 8 + bytePos);
        }
        lineBuf[bytesToRead] = '\0';

        /* Log newly arrived firmware trace */
        if (bytesToRead > 0) {
            CHAR bmsg[320];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[FW-c%u] %s\n", Core, lineBuf);
            if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
                SofLogBreadcrumb(bmsg);
            }
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", bmsg);
        }
    }

    /* Advance host read pointer in DSP SRAM */
    SofDspWrite32(sramBase, slotOffset, dspWritePtr);
    Ctx->FwLogHostReadPtr[Core] = dspWritePtr;
}

/**
 * @brief Configures and enables DSP firmware Zephyr logging over IPC4.
 *
 * Transmits system time synchronization (SOF_IPC4_FW_PARAM_SYSTEM_TIME) and log priority
 * configuration (SOF_IPC4_FW_PARAM_ENABLE_LOGS) to the Base FW module.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return NTSTATUS STATUS_SUCCESS if firmware logging was successfully armed.
 */
NTSTATUS
SofIpc4EnableFwLogs(
    _Inout_ PSOF_DEVICE_CONTEXT Ctx
)
{
    SOF_IPC_MESSAGE msg;
    SOF_IPC_MESSAGE reply;
    NTSTATUS status;
    LARGE_INTEGER sysTime;
    UINT64 usTime;

    struct {
        ULONG AgingTimerPeriod;
        ULONG FifoFullTimerPeriod;
        ULONG Enable;
        ULONG LogsPrioritiesMask[16];
    } logState;

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (Ctx->FwState != SOF_FW_BOOT_COMPLETE) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* 1. Synchronize system time with DSP firmware (SOF_IPC4_FW_PARAM_SYSTEM_TIME = 20) */
    KeQuerySystemTime(&sysTime);
    usTime = (UINT64)(sysTime.QuadPart / 10); /* Convert 100ns units to microseconds */

    RtlZeroMemory(&msg, sizeof(msg));
    RtlZeroMemory(&reply, sizeof(reply));

    msg.Primary = SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG) |
                  SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
                  SOF_IPC4_MOD_ID(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID) |
                  SOF_IPC4_MOD_INSTANCE(SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID);
    msg.Extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_FW_PARAM_SYSTEM_TIME);
    msg.Payload = &usTime;
    msg.PayloadSize = sizeof(usTime);

    status = SofIpcSendMessage(Ctx, &msg, &reply, 500);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Warning: Failed to sync system time with FW (0x%08x)\n", status);
    }

    /* 2. Configure log priorities and enable FW logs (SOF_IPC4_FW_PARAM_ENABLE_LOGS = 6) */
    RtlZeroMemory(&logState, sizeof(logState));
    logState.AgingTimerPeriod = 0x100;     /* 256 ms aging timer */
    logState.FifoFullTimerPeriod = 0x1000; /* 4096 ms FIFO timer */
    logState.Enable = 1;                   /* Enable logs */
    /* Enable levels: Critical, Error, Warning, Info + Infra, HAL, Module, Audio */
    logState.LogsPrioritiesMask[0] = 0x000001ef;

    RtlZeroMemory(&msg, sizeof(msg));
    RtlZeroMemory(&reply, sizeof(reply));

    msg.Primary = SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG) |
                  SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
                  SOF_IPC4_MOD_ID(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID) |
                  SOF_IPC4_MOD_INSTANCE(SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID);
    msg.Extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_FW_PARAM_ENABLE_LOGS);
    msg.Payload = &logState;
    msg.PayloadSize = sizeof(logState);

    status = SofIpcSendMessage(Ctx, &msg, &reply, 500);
    if (NT_SUCCESS(status)) {
        Ctx->FwLogInitialized = TRUE;
        SofLogBreadcrumb("[SOF] DSP Firmware Zephyr logging enabled successfully\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] DSP Firmware Zephyr logging enabled.\n");
    } else {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Warning: Failed to enable FW logging over IPC4 (0x%08x)\n", status);
    }

    return status;
}
