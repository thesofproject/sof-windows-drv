/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Loader.c
 * @brief Firmware Loader, Image Parser, and Stream DMA Staging Implementation.
 *
 * @details
 * Orchestrates the full lifecycle of loading, validating, staging, and executing
 * Sound Open Firmware binaries on Intel cAVS and ACE hardware:
 *  1. Image Parsing: Validates the extended manifest header ($AE1 magic), verifies
 *     bounds, strips the host manifest, and inspects module/version headers.
 *  2. DMA Memory Management: Allocates physical memory using KMDF Common Buffer
 *     (with full IOMMU / VT-d support) or contiguous physical memory fallback.
 *  3. CPU Cache Flushes: Flushes processor caches (KeFlushIoBuffers) to guarantee
 *     DRAM coherency before hardware DMA engines are triggered.
 *  4. Decoupled Code Loader DMA: Programs Intel HD-A stream registers (SD_CTL, SD_CBL,
 *     SD_FORMAT, SD_BDLPL/PU, SD_LVI) and decouples the stream from the physical link
 *     via Processing Pipe Control (PPCTL) and Software Position in Buffer (SPIB).
 *  5. DSP Power & Boot Sequencing: Powers down existing DSP context, asserts host IP
 *     ownership, powers on subsystem (HFDSSCS) and domain gating (HFPWRCTL), transmits
 *     ROM PURGE IPC with stream tag, powers Core 0 (DSP2CXCTL), and polls ROM status
 *     until FW_ENTERED and IPC4 FW_READY notifications are received.
 * Copyright 2026 The SOF Authors
 */

#ifdef _KERNEL_MODE
#include "Device.h"
#include <ntstrsafe.h>
void SofLogBreadcrumb(const char* text);
#else
#include <windows.h>
#include <strsafe.h>
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_INVALID_IMAGE_FORMAT
#define STATUS_INVALID_IMAGE_FORMAT      ((NTSTATUS)0xC000007BL)
#endif
#ifndef STATUS_INVALID_BUFFER_SIZE
#define STATUS_INVALID_BUFFER_SIZE       ((NTSTATUS)0xC0000206L)
#endif
#define DbgPrintEx(id, level, fmt, ...)  printf(fmt, ##__VA_ARGS__)
#define DPFLTR_DEFAULT_ID 0
#define DPFLTR_ERROR_LEVEL 0
#define DPFLTR_WARNING_LEVEL 0
#define DPFLTR_INFO_LEVEL 0
#endif
#include "Loader.h"
#include "Hw.h"
#include "Ipc.h"

/**
 * @brief Parses an SOF firmware binary and extracts manifest and version metadata.
 *
 * Checks:
 *  - Buffer length against minimum extended manifest header size.
 *  - Magic number equality with SOF_EXT_MAN4_MAGIC_NUMBER ($AE1 / 0x31454124).
 *  - Manifest length boundary within the binary image size.
 * Extracts the payload offset, payload length, module count, version numbers,
 * and ASCII binary name from the embedded base firmware header.
 *
 * @param[in]  Buffer Pointer to raw binary image in memory.
 * @param[in]  Size   Total size of binary image in bytes.
 * @param[out] Info   Pointer to SOF_FW_IMAGE_INFO structure to receive parsed metadata.
 * @return STATUS_SUCCESS on success; STATUS_INVALID_PARAMETER, STATUS_INVALID_IMAGE_FORMAT,
 *         or STATUS_INVALID_BUFFER_SIZE on validation error.
 */
NTSTATUS
SofParseFirmwareImage(
    _In_reads_bytes_(Size) const void *Buffer,
    _In_ size_t Size,
    _Out_ PSOF_FW_IMAGE_INFO Info
)
{
    const struct sof_ext_manifest4_hdr *extMan;
    const struct sof_man4_fw_binary_header *fwHeader;
    size_t minSize = sizeof(struct sof_ext_manifest4_hdr);

    if (!Buffer || !Info || Size < minSize) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Info, sizeof(SOF_FW_IMAGE_INFO));
    Info->RawData = (PVOID)Buffer;
    Info->RawSize = (ULONG)Size;

    extMan = (const struct sof_ext_manifest4_hdr *)Buffer;

    /*
     * Verify Extended Manifest Magic Number:
     * SOF v4 manifests start with 4-byte ASCII string "$AE1" (0x31454124).
     */
    if (extMan->id != SOF_EXT_MAN4_MAGIC_NUMBER) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Invalid extended manifest magic: 0x%08x\n", extMan->id);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    /* Verify declared manifest length does not exceed overall binary size */
    if (extMan->len >= Size) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Manifest length (0x%x) exceeds binary size (0x%zx)\n",
                   extMan->len, Size);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    /*
     * Payload Boundary:
     * The actual signed DSP executable payload begins immediately after
     * the host extended manifest header (extMan->len).
     */
    Info->PayloadOffset = extMan->len;
    Info->PayloadSize = (ULONG)(Size - extMan->len);
    Info->NumModules = extMan->num_module_entries;

    /*
     * Inspect Base Firmware Binary Header (if present at the start of payload):
     * Contains version numbers, build counters, and human-readable binary name.
     */
    if (Size >= (size_t)extMan->len + sizeof(struct sof_man4_fw_binary_header)) {
        fwHeader = (const struct sof_man4_fw_binary_header *)((const UCHAR *)Buffer + extMan->len);
        Info->MajorVersion = fwHeader->major_version;
        Info->MinorVersion = fwHeader->minor_version;
        Info->HotfixVersion = fwHeader->hotfix_version;
        Info->BuildVersion = fwHeader->build_version;
        Info->PreloadPageCount = fwHeader->preload_page_count;

        RtlCopyMemory(Info->BinaryName, fwHeader->name, min(sizeof(Info->BinaryName) - 1, sizeof(fwHeader->name)));
        Info->BinaryName[sizeof(Info->BinaryName) - 1] = '\0';

        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Firmware Image: %s, Version %u.%u.%u.%u, Payload Size 0x%x\n",
                   Info->BinaryName,
                   Info->MajorVersion,
                   Info->MinorVersion,
                   Info->HotfixVersion,
                   Info->BuildVersion,
                   Info->PayloadSize);
    } else {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Extended manifest verified, payload size: 0x%x bytes\n",
                   Info->PayloadSize);
    }

    return STATUS_SUCCESS;
}

#ifdef _KERNEL_MODE

/**
 * @brief Reads a complete firmware file from the filesystem into non-paged kernel pool.
 *
 * Uses ZwOpenFile, ZwQueryInformationFile, and ZwReadFile. Opens the file with
 * SYNCHRONIZE and FILE_SYNCHRONOUS_IO_NONALERT flags to read synchronously at PASSIVE_LEVEL.
 *
 * @param[in]  FilePath   Fully-qualified NT object path to firmware file.
 * @param[out] Buffer     Pointer to receive allocated memory buffer containing file data.
 * @param[out] Size       Pointer to receive file size in bytes.
 * @return STATUS_SUCCESS on success, or an NTSTATUS failure code.
 */
NTSTATUS
SofLoadFirmwareFile(
    _In_ PCWSTR FilePath,
    _Out_ PVOID *Buffer,
    _Out_ PULONG Size
)
{
    UNICODE_STRING uniName;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatus;
    HANDLE fileHandle = NULL;
    FILE_STANDARD_INFORMATION fileInfo;
    PVOID rawBuffer = NULL;
    ULONG fileSize = 0;
    NTSTATUS status;

    if (!FilePath || !Buffer || !Size)
        return STATUS_INVALID_PARAMETER;

    *Buffer = NULL;
    *Size = 0;

    RtlInitUnicodeString(&uniName, FilePath);
    InitializeObjectAttributes(&objAttr,
                               &uniName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    status = ZwOpenFile(&fileHandle,
                        GENERIC_READ | SYNCHRONIZE,
                        &objAttr,
                        &ioStatus,
                        FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT);

    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Failed to open firmware file %ls: 0x%08x\n",
                   FilePath, status);
        return status;
    }

    status = ZwQueryInformationFile(fileHandle,
                                    &ioStatus,
                                    &fileInfo,
                                    sizeof(fileInfo),
                                    FileStandardInformation);

    if (!NT_SUCCESS(status)) {
        ZwClose(fileHandle);
        return status;
    }

    fileSize = fileInfo.EndOfFile.LowPart;
    if (fileSize == 0 || fileInfo.EndOfFile.HighPart != 0) {
        ZwClose(fileHandle);
        return STATUS_FILE_INVALID;
    }

    rawBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, fileSize, SOF_POOL_TAG);
    if (!rawBuffer) {
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwReadFile(fileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &ioStatus,
                        rawBuffer,
                        fileSize,
                        NULL,
                        NULL);

    ZwClose(fileHandle);

    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(rawBuffer, SOF_POOL_TAG);
        return status;
    }

    *Buffer = rawBuffer;
    *Size = fileSize;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Successfully loaded firmware file %ls (%u bytes)\n",
               FilePath, fileSize);

    return STATUS_SUCCESS;
}

/**
 * @brief Frees a kernel memory buffer allocated by SofLoadFirmwareFile.
 *
 * @param[in] Buffer Pointer to buffer to free.
 */
VOID
SofFreeFirmwareBuffer(
    _In_ PVOID Buffer
)
{
    if (Buffer) {
        ExFreePoolWithTag(Buffer, SOF_POOL_TAG);
    }
}

/**
 * @brief Allocates a physically contiguous or IOMMU-mapped DMA buffer for bus mastering.
 *
 * Dual-Strategy Allocation:
 *  1. Primary: Uses KMDF Common Buffer (WdfCommonBufferCreate). Essential on systems with
 *     IOMMU / Intel VT-d enabled (e.g. DMA Protection, Virtualization-Based Security), as
 *     it guarantees device-visible logical address programming.
 *  2. Fallback: Uses MmAllocateContiguousNodeMemory constrained to < 4GB address space,
 *     falling back to 64-bit space if 32-bit physical RAM is exhausted.
 *
 * @param[in]  Ctx    Pointer to the SOF device context.
 * @param[in]  Size   Allocation size in bytes.
 * @param[out] DmaBuf Pointer to SOF_DMA_BUFFER structure receiving memory handles and addresses.
 * @return STATUS_SUCCESS on success, or STATUS_INSUFFICIENT_RESOURCES on failure.
 */
NTSTATUS
SofAllocateDmaBuffer(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG Size,
    _Out_ PSOF_DMA_BUFFER DmaBuf
)
{
    NTSTATUS status;

    if (!Ctx || !DmaBuf || Size == 0)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(DmaBuf, sizeof(SOF_DMA_BUFFER));
    DmaBuf->Size = Size;

    /* Primary: Use KMDF Common Buffer for hardware IOMMU (VT-d) mapping */
    if (Ctx->DmaEnabler != NULL) {
        WDFCOMMONBUFFER commonBuf = NULL;
        status = WdfCommonBufferCreate(Ctx->DmaEnabler, Size, WDF_NO_OBJECT_ATTRIBUTES, &commonBuf);
        if (NT_SUCCESS(status) && commonBuf != NULL) {
            char bmsg[128];
            DmaBuf->CommonBuffer = commonBuf;
            DmaBuf->VirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(commonBuf);
            DmaBuf->PhysicalAddress = WdfCommonBufferGetAlignedLogicalAddress(commonBuf);
            RtlZeroMemory(DmaBuf->VirtualAddress, Size);
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[12-DMA] WdfCommonBuffer: VA=%p, LA=0x%llx, Size=0x%x\n",
                DmaBuf->VirtualAddress, DmaBuf->PhysicalAddress.QuadPart, Size);
            SofLogBreadcrumb(bmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] WdfCommonBuffer allocated: VA=%p, LA=0x%llx, Size=0x%x\n",
                       DmaBuf->VirtualAddress,
                       DmaBuf->PhysicalAddress.QuadPart,
                       Size);
            return STATUS_SUCCESS;
        } else {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[12-DMA-WARN] WdfCommonBufferCreate failed: 0x%08x, falling back to contiguous\n", status);
            SofLogBreadcrumb(bmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                       "[SOF] WdfCommonBufferCreate failed (0x%08x), falling back to contiguous memory\n",
                       status);
        }
    } else {
        SofLogBreadcrumb("[12-DMA-WARN] Ctx->DmaEnabler is NULL! Falling back to contiguous memory\n");
    }

    /* Fallback: Contiguous physical allocation */
    PHYSICAL_ADDRESS lowest = { .QuadPart = 0 };
    PHYSICAL_ADDRESS highest = { .QuadPart = 0xFFFFFFFFULL }; /* Prefer < 4GB first */
    PHYSICAL_ADDRESS boundary = { .QuadPart = 0 };

    DmaBuf->VirtualAddress = MmAllocateContiguousNodeMemory(Size,
                                                           lowest,
                                                           highest,
                                                           boundary,
                                                           PAGE_READWRITE,
                                                           MM_ANY_NODE_OK);
    if (!DmaBuf->VirtualAddress) {
        highest.QuadPart = 0xFFFFFFFFFFFFFFFFULL;
        DmaBuf->VirtualAddress = MmAllocateContiguousNodeMemory(Size,
                                                               lowest,
                                                               highest,
                                                               boundary,
                                                               PAGE_READWRITE,
                                                               MM_ANY_NODE_OK);
    }

    if (!DmaBuf->VirtualAddress) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Failed to allocate contiguous DMA buffer (%u bytes)\n",
                   Size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(DmaBuf->VirtualAddress, Size);
    DmaBuf->PhysicalAddress = MmGetPhysicalAddress(DmaBuf->VirtualAddress);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Fallback DMA Buffer allocated: VA=%p, PA=0x%llx, Size=0x%x\n",
               DmaBuf->VirtualAddress,
               DmaBuf->PhysicalAddress.QuadPart,
               Size);

    return STATUS_SUCCESS;
}

/**
 * @brief Releases memory associated with a SOF_DMA_BUFFER structure.
 *
 * Handles both KMDF Common Buffer deletion and MmFreeContiguousMemory.
 *
 * @param[in]    Ctx    Pointer to the SOF device context.
 * @param[in,out] DmaBuf Pointer to SOF_DMA_BUFFER descriptor to free.
 */
VOID
SofFreeDmaBuffer(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _Inout_ PSOF_DMA_BUFFER DmaBuf
)
{
    UNREFERENCED_PARAMETER(Ctx);

    if (!DmaBuf || !DmaBuf->VirtualAddress)
        return;

    if (DmaBuf->CommonBuffer != NULL) {
        WdfObjectDelete(DmaBuf->CommonBuffer);
        DmaBuf->CommonBuffer = NULL;
    } else {
        MmFreeContiguousMemory(DmaBuf->VirtualAddress);
    }

    DmaBuf->VirtualAddress = NULL;
    DmaBuf->PhysicalAddress.QuadPart = 0;
    DmaBuf->Size = 0;
}

/**
 * @brief Powers down the DSP subsystem, core 0, and gates domain power on Intel ACE.
 *
 * Power Down Sequence on ACE:
 *  1. Reclaims IP & SHIM ownership for the Host CPU (OSEL = 0x2).
 *  2. Powers down Primary Core (clears SPA in MTL_DSP2CXCTL_PRIMARY_CORE) and polls CPA.
 *  3. Gates domain power (clears WPDSPHPXPG in PTL_HFPWRCTL2 / MTL_HFPWRCTL) and polls HFPWRSTS.
 *  4. Powers down DSP subsystem (clears SPA in MTL_HFDSSCS) and polls CPA.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID
SofPowerDownDsp(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    PUCHAR bar4;
    ULONG timeout;
    char bmsg[128];

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return;

    bar4 = (PUCHAR)Ctx->Bar4VirtualAddress;

    if (Ctx->HwDesc->IsAce) {

    /*
     * 1. Set Host CPU ownership and power down Primary Core:
     * OSEL = 0x2 gives Host CPU ownership of IP & shim registers.
     * Clearing SPA requests core power-off.
     */
    SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                     MTL_DSP2CXCTL_PRIMARY_CORE_OSEL,
                     0x2 << MTL_DSP2CXCTL_PRIMARY_CORE_OSEL_SHIFT);
    SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                     MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK, 0);
    KeStallExecutionProcessor(1000);
    timeout = 5000;
    while (timeout--) {
        ULONG ctl = SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE);
        if (ctl != 0xFFFFFFFF && !(ctl & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK))
            break;
        KeStallExecutionProcessor(10);
    }

    /*
     * 2. Gate domain power:
     * Clear WPDSPHPXPG bit to gate power to the DSP high-performance domain.
     */
    ULONG pwrCtl = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRCTL2 : MTL_HFPWRCTL;
    ULONG pwrSts = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRSTS2 : MTL_HFPWRSTS;
    SofDspUpdateBits(bar4, pwrCtl, MTL_HFPWRCTL_WPDSPHPXPG, 0);
    KeStallExecutionProcessor(1000);
    timeout = 5000;
    while (timeout--) {
        ULONG psts = SofDspRead32(bar4, pwrSts);
        if (psts != 0xFFFFFFFF && !(psts & MTL_HFPWRSTS_DSPHPXPGS_MASK))
            break;
        KeStallExecutionProcessor(10);
    }

    /*
     * 3. Power down DSP Subsystem:
     * Clear SPA in HFDSSCS to power down the entire audio DSP subsystem.
     */
    SofDspUpdateBits(bar4, MTL_HFDSSCS, MTL_HFDSSCS_SPA_MASK, 0);
    KeStallExecutionProcessor(1000);
    timeout = 5000;
    while (timeout--) {
        ULONG dsscs = SofDspRead32(bar4, MTL_HFDSSCS);
        if (dsscs != 0xFFFFFFFF && !(dsscs & MTL_HFDSSCS_CPA_MASK))
            break;
        KeStallExecutionProcessor(10);
    }

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12-PWR] DSP powered down: Core0 CPA=%s, Subsystem CPA=%s, ROM_STS=0x%08x\n",
        (SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE) & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK) ? "1" : "0",
        (SofDspRead32(bar4, MTL_HFDSSCS) & MTL_HFDSSCS_CPA_MASK) ? "1" : "0",
        SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
    SofLogBreadcrumb(bmsg);
    } else {
        /*
         * cAVS power-down: Stall and reset Core 0, then clear SPA in ADSPCS.
         */
        SofDspUpdateBits(bar4, HDA_DSP_REG_ADSPCS,
                         HDA_DSP_ADSPCS_CSTALL_MASK(1),
                         HDA_DSP_ADSPCS_CSTALL_MASK(1));
        SofDspUpdateBits(bar4, HDA_DSP_REG_ADSPCS,
                         HDA_DSP_ADSPCS_CRST_MASK(1),
                         HDA_DSP_ADSPCS_CRST_MASK(1));
        SofDspUpdateBits(bar4, HDA_DSP_REG_ADSPCS,
                         HDA_DSP_ADSPCS_SPA_MASK(1), 0);
        KeStallExecutionProcessor(1000);
        timeout = 5000;
        while (timeout--) {
            ULONG adspcs = SofDspRead32(bar4, HDA_DSP_REG_ADSPCS);
            if (adspcs != 0xFFFFFFFF && !(adspcs & HDA_DSP_ADSPCS_CPA_MASK(1)))
                break;
            KeStallExecutionProcessor(10);
        }

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12-PWR] cAVS DSP powered down: ADSPCS=0x%08x, ROM_STS=0x%08x\n",
            SofDspRead32(bar4, HDA_DSP_REG_ADSPCS),
            SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
        SofLogBreadcrumb(bmsg);
    }
}

/**
 * @brief Configures HD-A Code Loader decoupled DMA registers and Buffer Descriptor List.
 *
 * Implementation Steps:
 *  1. Verifies HD-A Global Controller is active (GCTL.CRST == 1).
 *  2. Determines Code Loader stream index (first playback stream = NumCaptureStreams).
 *  3. Decouples the stream from the physical HD-A link in PPCTL.
 *  4. Builds a 2-entry Buffer Descriptor List (BDL) mapping the firmware payload into
 *     two halves (HD-A requires LVI >= 1). Sets Interrupt on Completion (IOC) on entry 1.
 *  5. Flushes processor write caches (KeFlushIoBuffers) for staging payload and BDL.
 *  6. Performs hardware stream engine reset cycle (assert SRST, poll, clear SRST, poll).
 *  7. Clears status flags (BCIS, FIFOE, DESE).
 *  8. Programs Cyclic Buffer Length (SD_CBL), Stream Format (SD_FORMAT = 0x40, 32-bit,
 *     48kHz, mono), Last Valid Index (SD_LVI = 1), and BDL base addresses (SD_BDLPL/PU).
 *  9. Programs stream tag (tag 1) and enables stream interrupt in SD_CTL and INTCTL.
 * 10. Programs Software Position in Buffer (SPIB) credit to allow decoupled DMA engine
 *     to stream the entire buffer without early wrap-around.
 *
 * @param[in]  Ctx            Pointer to the SOF device context.
 * @param[in]  FwDmaBuf       Pointer to DMA buffer containing stripped firmware payload.
 * @param[in]  BdlDmaBuf      Pointer to DMA buffer containing the 2-entry BDL table.
 * @param[in]  StreamTag      Stream tag number to program in SD_CTL (typically 1).
 * @param[out] OutStreamIndex Optional pointer to receive selected stream index.
 * @return STATUS_SUCCESS on success, or an NTSTATUS failure code.
 */
NTSTATUS
SofConfigureCodeLoaderDma(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PSOF_DMA_BUFFER FwDmaBuf,
    _In_ PSOF_DMA_BUFFER BdlDmaBuf,
    _In_ ULONG StreamTag,
    _Out_opt_ PULONG OutStreamIndex
)
{
    PUCHAR bar0;
    PUCHAR streamRegs;
    PSOF_INTEL_DSP_BDL bdl;
    ULONG timeout = 500;
    ULONG ctl;
    ULONG halfSize;
    ULONG restSize;
    ULONG streamIndex;
    ULONG sdOffset;
    ULONG gctl;
    char bmsg[256];

    if (!Ctx->Bar0VirtualAddress || !FwDmaBuf || !BdlDmaBuf)
        return STATUS_INVALID_PARAMETER;

    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;

    /*
     * Ensure HD-A Controller is out of reset (GCTL.CRST == 1).
     * If the controller is in reset, DMA engines cannot accept commands or cycle.
     */
    gctl = readl(bar0 + SOF_HDA_GCTL);
    if (!(gctl & SOF_HDA_GCTL_RESET)) {
        writel(gctl | SOF_HDA_GCTL_RESET, bar0 + SOF_HDA_GCTL);
        KeStallExecutionProcessor(1000);
        gctl = readl(bar0 + SOF_HDA_GCTL);
    }

    /*
     * Select Code Loader DMA Stream:
     * In Intel HD-A architectures, the Code Loader engine is mapped to the first
     * Playback stream descriptor, which immediately follows the capture streams
     * (streamIndex = NumCaptureStreams).
     */
    streamIndex = Ctx->NumCaptureStreams;
    if (streamIndex == 0) streamIndex = 7; /* Fallback for typical 7-capture configs */
    if (OutStreamIndex) *OutStreamIndex = streamIndex;

    sdOffset = SOF_HDA_ADSP_LOADER_BASE + (streamIndex * SOF_HDA_ADSP_SD_ENTRY_SIZE);
    streamRegs = bar0 + sdOffset;

    /*
     * Decouple Stream from Link DMA:
     * Set decouple bit (1 << streamIndex) in Processing Pipe Control (PPCTL) so the
     * stream routes directly to the DSP internal FIFO instead of the external audio link.
     */
    if (Ctx->PpCapOffset) {
        writel(readl(bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL) | (1 << streamIndex) | SOF_HDA_PPCTL_GPROCEN | SOF_HDA_PPCTL_PIE,
               bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL);
    }

    /*
     * Build 2-entry Buffer Descriptor List (BDL):
     * The Intel HD-A specification strictly requires Last Valid Index (LVI) >= 1.
     * We divide the firmware payload into two equal halves (128-byte aligned).
     */
    halfSize = (FwDmaBuf->Size / 2) & ~0x7F;
    restSize = FwDmaBuf->Size - halfSize;
    bdl = (PSOF_INTEL_DSP_BDL)BdlDmaBuf->VirtualAddress;

    /* Entry 0: First half of firmware image */
    bdl[0].AddrLow = FwDmaBuf->PhysicalAddress.LowPart;
    bdl[0].AddrHigh = FwDmaBuf->PhysicalAddress.HighPart;
    bdl[0].Size = halfSize;
    bdl[0].Ioc = 0;

    /* Entry 1: Second half of firmware image, with IOC (Interrupt On Completion) set */
    bdl[1].AddrLow = FwDmaBuf->PhysicalAddress.LowPart + halfSize;
    bdl[1].AddrHigh = FwDmaBuf->PhysicalAddress.HighPart;
    bdl[1].Size = restSize;
    bdl[1].Ioc = 1;

    /*
     * Flush Processor Caches to System RAM:
     * Guarantees that the CPU write buffers and L1/L2/L3 cache lines holding the
     * payload and BDL table are written back to DRAM before the DMA engine reads them.
     */
    {
        PMDL mdl = IoAllocateMdl(FwDmaBuf->VirtualAddress, FwDmaBuf->Size, FALSE, FALSE, NULL);
        if (mdl) {
            MmBuildMdlForNonPagedPool(mdl);
            KeFlushIoBuffers(mdl, TRUE, TRUE);
            IoFreeMdl(mdl);
        }
        PMDL bdlMdl = IoAllocateMdl(BdlDmaBuf->VirtualAddress, BdlDmaBuf->Size, FALSE, FALSE, NULL);
        if (bdlMdl) {
            MmBuildMdlForNonPagedPool(bdlMdl);
            KeFlushIoBuffers(bdlMdl, TRUE, TRUE);
            IoFreeMdl(bdlMdl);
        }
    }

    /*
     * 1. Reset Stream Engine:
     * Assert SRST in SD_CTL, poll for hardware confirmation, then clear SRST and poll.
     */
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL), SOF_HDA_SD_CTL_DMA_RESET);
    while (timeout--) {
        ctl = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL));
        if (ctl & SOF_HDA_SD_CTL_DMA_RESET)
            break;
        KeStallExecutionProcessor(10);
    }

    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL), 0);
    timeout = 500;
    while (timeout--) {
        ctl = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL));
        if (!(ctl & SOF_HDA_SD_CTL_DMA_RESET))
            break;
        KeStallExecutionProcessor(10);
    }

    /* 2. Clear Status Flags: BCIS (buffer completion), FIFOE (FIFO error), DESE (descriptor error) */
    WRITE_REGISTER_UCHAR((PUCHAR)(streamRegs + SOF_HDA_ADSP_REG_SD_STS), 0x1C);

    /* 3. Program Cyclic Buffer Length (total size of payload) */
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CBL), FwDmaBuf->Size);

    /* 4. Program Stream Format: HDA_CL_STREAM_FORMAT = 0x40 (32-bit, 48kHz, 1 channel) */
    WRITE_REGISTER_USHORT((PUSHORT)(streamRegs + SOF_HDA_ADSP_REG_SD_FORMAT), SOF_HDA_CL_STREAM_FORMAT);

    /* 5. Program Last Valid Index (2 entries => index 1) */
    WRITE_REGISTER_USHORT((PUSHORT)(streamRegs + SOF_HDA_ADSP_REG_SD_LVI), 1);

    /* 6. Program BDL Physical Base Address (64-bit split across BDLPL and BDLPU) */
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL),
                         BdlDmaBuf->PhysicalAddress.LowPart);
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPU),
                         BdlDmaBuf->PhysicalAddress.HighPart);

    /* 7. Program Stream Tag and Interrupt Mask in SD_CTL */
    ctl = (StreamTag << 20) | SOF_HDA_CL_DMA_SD_INT_MASK;
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL), ctl);

    /* 8. Enable stream interrupt & Global Interrupt in SOF_HDA_INTCTL */
    writel(readl(bar0 + SOF_HDA_INTCTL) | (1 << streamIndex) | SOF_HDA_INT_GLOBAL_EN | SOF_HDA_INT_CTRL_EN,
           bar0 + SOF_HDA_INTCTL);

    /*
     * 9. Configure Software Position in Buffer (SPIB) credit for decoupled DMA:
     * For decoupled streams, HD-A hardware uses SPIB to track available FIFO credits.
     * We enable SPIB in SPBFCCTL and write the entire buffer size into the SPIB register.
     */
    if (Ctx->SpbCapOffset) {
        PUCHAR spibBar = bar0 + Ctx->SpbCapOffset;
        PUCHAR spibAddr = spibBar + SOF_HDA_SPIB_BASE + (streamIndex * SOF_HDA_SPIB_INTERVAL) + SOF_HDA_SPIB_SPIB;

        /* Enable SPIB in SPBFCCTL (bit = 1 << streamIndex) */
        writel(readl(spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL) | (1 << streamIndex),
               spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);

        /* Program software position in buffer to full buffer size */
        writel(FwDmaBuf->Size, spibAddr);

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12-SPIB] SPBFCCTL=0x%08x, SPIB[%u]=0x%x (addr=%p)\n",
            readl(spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL),
            streamIndex, readl(spibAddr), spibAddr);
        SofLogBreadcrumb(bmsg);
    }

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12b1] DMA Stream: GCTL=0x%08x, PPCTL=0x%08x, INTCTL=0x%08x, SD_CTL=0x%08x, CBL=0x%x, LVI=%u, BDL=0x%llx\n",
        gctl,
        Ctx->PpCapOffset ? readl(bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL) : 0,
        readl(bar0 + SOF_HDA_INTCTL),
        READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL)),
        READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CBL)),
        READ_REGISTER_USHORT((PUSHORT)(streamRegs + SOF_HDA_ADSP_REG_SD_LVI)),
        BdlDmaBuf->PhysicalAddress.QuadPart);
    SofLogBreadcrumb(bmsg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Code Loader DMA programmed: StreamIndex=%u (offset 0x%x), StreamTag=%u, BufferSize=0x%x\n",
               streamIndex, sdOffset, StreamTag, FwDmaBuf->Size);

    return STATUS_SUCCESS;
}

/**
 * @brief Complete DSP firmware load, hardware initialization, and execution orchestrator.
 *
 * Sequence of Operations:
 *  1. Determines target binary file path based on microarchitecture family (PTL, LNL, MTL, etc.).
 *  2. Loads raw image from filesystem and parses extended manifest ($AE1).
 *  3. Allocates physical DMA staging buffer and 4KB BDL descriptor buffer.
 *  4. Powers down any active DSP state (SofPowerDownDsp).
 *  5. Programs HD-A Code Loader stream registers and SPIB credit.
 *  6. Powers on DSP Subsystem (HFDSSCS) and un-gates domain power (HFPWRCTL).
 *  7. Sends ROM IPC PURGE command with stream tag 1 via HIPCIDR and waits for ROM ACK.
 *  8. Powers up Primary Core 0 (DSP2CXCTL) and sets Host CPU IP ownership.
 *  9. Unmasks Host IPC interrupts in DSP SHIM.
 * 10. Awaits ROM status transition to FSR_STATE_INIT_DONE (0x1).
 * 11. Triggers Code Loader DMA stream RUN bit (SD_CTL.DMA_START).
 * 12. Polls ROM status until FSR_STATE_FW_ENTERED (0x5) is reached.
 * 13. Halts Code Loader DMA and cleans up staging DMA buffers.
 * 14. Awaits SOF_IPC4_NOTIFY_FW_READY notification from running firmware.
 *
 * @param[in,out] Ctx             Pointer to the SOF device context.
 * @param[in,opt] OptionalFwPath  Optional custom path to firmware binary; NULL for default path.
 * @return STATUS_SUCCESS on successful boot, or an NTSTATUS error code.
 */
NTSTATUS
SofStartFirmwareBoot(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_opt_ PCWSTR OptionalFwPath
)
{
    SOF_DMA_BUFFER fwDma = {0};
    SOF_DMA_BUFFER bdlDma = {0};
    SOF_FW_IMAGE_INFO fwInfo = {0};
    PVOID rawFw = NULL;
    ULONG rawFwSize = 0;
    WCHAR defaultPath[128];
    PCWSTR targetPath = OptionalFwPath;
    PUCHAR bar4;
    PUCHAR bar0;
    ULONG streamTag = 1;
    ULONG streamIndex = 7;
    ULONG sdOffset;
    PUCHAR streamRegs;
    ULONG ipcHdr;
    ULONG statusReg = 0;
    ULONG timeout;
    char bmsg[128];
    NTSTATUS status;

    if (!Ctx || !Ctx->Bar0VirtualAddress || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return STATUS_DEVICE_NOT_READY;

    /* Reset firmware state & clear events for fresh boot / resume re-boot */
    Ctx->FwState = SOF_FW_BOOT_NOT_STARTED;
    KeClearEvent(&Ctx->FwReadyEvent);
    Ctx->IpcTxBusy = FALSE;

    bar4 = (PUCHAR)Ctx->Bar4VirtualAddress;
    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;

    /* Determine firmware filename if not specified */
    if (!targetPath) {
        PCSTR socName = "tgl";
        if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0) socName = (Ctx->HwDesc->CoresNum == 2) ? "nvl-s" : "nvl";
        else if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) socName = "ptl";
        else if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_2_0) socName = "lnl";
        else if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_1_0) socName = "mtl";
        else if (Ctx->HwDesc->Family == SOF_HW_INTEL_CAVS_2_0) socName = "icl";
        else if (Ctx->HwDesc->Family == SOF_HW_INTEL_CAVS_1_8) socName = "cnl";
        else if (Ctx->HwDesc->Family == SOF_HW_INTEL_CAVS_1_5) socName = "apl";

        RtlStringCbPrintfW(defaultPath, sizeof(defaultPath),
                           L"\\SystemRoot\\System32\\drivers\\intel\\sof\\sof-%S.ri",
                           socName);
        targetPath = defaultPath;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Initiating firmware boot from: %ls\n", targetPath);

    status = SofLoadFirmwareFile(targetPath, &rawFw, &rawFwSize);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Firmware binary not found at %ls (Status: 0x%08x)\n",
                   targetPath, status);
        return status;
    }

    status = SofParseFirmwareImage(rawFw, rawFwSize, &fwInfo);
    if (!NT_SUCCESS(status)) {
        SofFreeFirmwareBuffer(rawFw);
        return status;
    }

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12a] Firmware file loaded: %ls (raw: %u bytes, payload: 0x%x)\n",
        targetPath, rawFwSize, fwInfo.PayloadSize);
    SofLogBreadcrumb(bmsg);

    /* Allocate Contiguous DMA Buffer for Firmware Payload */
    status = SofAllocateDmaBuffer(Ctx, fwInfo.PayloadSize, &fwDma);
    if (!NT_SUCCESS(status)) {
        char err[128];
        RtlStringCbPrintfA(err, sizeof(err), "[12-ERR] Failed to allocate FW DMA buffer: 0x%08x\n", status);
        SofLogBreadcrumb(err);
        SofFreeFirmwareBuffer(rawFw);
        return status;
    }

    /* Allocate BDL Descriptor List Buffer (4096 bytes) */
    status = SofAllocateDmaBuffer(Ctx, 4096, &bdlDma);
    if (!NT_SUCCESS(status)) {
        char err[128];
        RtlStringCbPrintfA(err, sizeof(err), "[12-ERR] Failed to allocate BDL DMA buffer: 0x%08x\n", status);
        SofLogBreadcrumb(err);
        SofFreeDmaBuffer(Ctx, &fwDma);
        SofFreeFirmwareBuffer(rawFw);
        return status;
    }

    /* Copy stripped payload into DMA staging memory */
    RtlCopyMemory(fwDma.VirtualAddress,
                  (PUCHAR)rawFw + fwInfo.PayloadOffset,
                  fwInfo.PayloadSize);

    /* Free raw file copy */
    SofFreeFirmwareBuffer(rawFw);

    /* 0. Ensure DSP is powered down and reset before configuring DMA */
    SofPowerDownDsp(Ctx);

    /* Configure Code Loader stream on BAR0 */
    status = SofConfigureCodeLoaderDma(Ctx, &fwDma, &bdlDma, streamTag, &streamIndex);
    if (!NT_SUCCESS(status)) {
        char err[128];
        RtlStringCbPrintfA(err, sizeof(err), "[12-ERR] SofConfigureCodeLoaderDma failed: 0x%08x\n", status);
        SofLogBreadcrumb(err);
        SofFreeDmaBuffer(Ctx, &bdlDma);
        SofFreeDmaBuffer(Ctx, &fwDma);
        return status;
    }

    sdOffset = SOF_HDA_ADSP_LOADER_BASE + (streamIndex * SOF_HDA_ADSP_SD_ENTRY_SIZE);
    streamRegs = bar0 + sdOffset;

    /*
     * 1. Power Up DSP Subsystem & Domain Gating on ACE:
     * Step 1a: Power ON DSP Subsystem (MTL_HFDSSCS: SPA bit 16).
     * Step 1b: Un-gate DSP-0 Domain (PTL_HFPWRCTL2 / MTL_HFPWRCTL: bit 0).
     */
    if (Ctx->HwDesc->IsAce) {
        ULONG pwrCtl = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRCTL2 : MTL_HFPWRCTL;
        ULONG pwrSts = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRSTS2 : MTL_HFPWRSTS;

        /* Step 1a: Power ON DSP Subsystem */
        SofDspUpdateBits(bar4, MTL_HFDSSCS, MTL_HFDSSCS_SPA_MASK, MTL_HFDSSCS_SPA_MASK);
        KeStallExecutionProcessor(1000);
        timeout = 5000;
        while (timeout--) {
            ULONG dsscs = SofDspRead32(bar4, MTL_HFDSSCS);
            if (dsscs != 0xFFFFFFFF && (dsscs & MTL_HFDSSCS_CPA_MASK))
                break;
            KeStallExecutionProcessor(10);
        }

        /* Step 1b: Un-gate DSP-0 Domain */
        SofDspUpdateBits(bar4, pwrCtl, MTL_HFPWRCTL_WPDSPHPXPG, MTL_HFPWRCTL_WPDSPHPXPG);
        KeStallExecutionProcessor(1000);
        timeout = 5000;
        while (timeout--) {
            ULONG psts = SofDspRead32(bar4, pwrSts);
            if (psts != 0xFFFFFFFF && (psts & MTL_HFPWRSTS_DSPHPXPGS_MASK))
                break;
            KeStallExecutionProcessor(10);
        }

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12b0] Subsystem & domain power: HFDSSCS=0x%08x, PWRSTS=0x%08x, ROM_STS=0x%08x\n",
            SofDspRead32(bar4, MTL_HFDSSCS), SofDspRead32(bar4, pwrSts),
            SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
        SofLogBreadcrumb(bmsg);
    }

    /*
     * 2. Send ROM Control PURGE command with stream tag:
     * Informs DSP ROM which HD-A stream tag contains the firmware binary.
     */
    ipcHdr = HDA_DSP_ROM_IPC_CONTROL | HDA_DSP_ROM_IPC_PURGE_FW |
             ((streamTag - 1) << 9) | Ctx->HwDesc->HipcIdrBusy;

    SofDspWrite32(bar4, Ctx->HwDesc->HipcIdr, ipcHdr);

    /*
     * 3. Power Up DSP Primary Core:
     * On ACE: Set Host CPU IP ownership (OSEL = 0x2) and set SPA bit.
     * On cAVS: Set Core 0 SPA and release Reset/Stall in ADSPCS.
     */
    if (Ctx->HwDesc->IsAce) {
        /* ACE: Program owner of IP & shim registers to Host CPU (OSEL = 0x2) */
        SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                         MTL_DSP2CXCTL_PRIMARY_CORE_OSEL,
                         0x2 << MTL_DSP2CXCTL_PRIMARY_CORE_OSEL_SHIFT);

        /* Enable Primary Core SPA bit */
        SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                         MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK,
                         MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK);

        /* Wait 1ms for CPA to settle */
        KeStallExecutionProcessor(1000);

        /* Poll for CPA confirmation (bit 8, ensure not 0xFFFFFFFF) */
        timeout = 5000;
        while (timeout--) {
            ULONG ctlVal = SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE);
            if (ctlVal != 0xFFFFFFFF && (ctlVal & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK))
                break;
            KeStallExecutionProcessor(10);
        }

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12b] Core 0 power up: DSP2CXCTL=0x%08x (CPA=%s)\n",
            SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE),
            (SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE) != 0xFFFFFFFF &&
             (SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE) & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK)) ? "SET" : "TIMEOUT");
        SofLogBreadcrumb(bmsg);
    } else {
        /* cAVS: Set Core 0 SPA and release Reset/Stall */
        SofDspUpdateBits(bar4, HDA_DSP_REG_ADSPCS,
                         HDA_DSP_ADSPCS_SPA_MASK(1),
                         HDA_DSP_ADSPCS_SPA_MASK(1));

        /* Release reset and unstall Core 0 */
        SofDspUpdateBits(bar4, HDA_DSP_REG_ADSPCS,
                         HDA_DSP_ADSPCS_CRST_MASK(1),
                         HDA_DSP_ADSPCS_CRST_MASK(1));

        SofDspUpdateBits(bar4, HDA_DSP_REG_ADSPCS,
                         HDA_DSP_ADSPCS_CSTALL_MASK(1),
                         0);

        SofDspUpdateBits(bar4, HDA_DSP_REG_ADSPCS,
                         HDA_DSP_ADSPCS_CRST_MASK(1),
                         0);
    }

    /* Wait for ROM IPC ACK in HIPCIDA */
    timeout = 2000;
    while (timeout--) {
        ULONG ack = SofDspRead32(bar4, Ctx->HwDesc->HipcIda);
        if (ack & Ctx->HwDesc->HipcIdaDone)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &(LARGE_INTEGER){.QuadPart = -10000LL}); /* 1ms */
    }

    /* Acknowledge ROM */
    SofDspWrite32(bar4, Ctx->HwDesc->HipcIda, Ctx->HwDesc->HipcIdaDone);

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12c] ROM PURGE ACK: IDA=0x%08x, ROM_STS=0x%08x\n",
        SofDspRead32(bar4, Ctx->HwDesc->HipcIda),
        SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
    SofLogBreadcrumb(bmsg);

    /* 4. Enable Host IPC Interrupts in DSP */
    if (Ctx->HwDesc->IsAce && Ctx->HwDesc->IntIpPtrReg) {
        ULONG ptr = SofDspRead32(bar4, Ctx->HwDesc->IntIpPtrReg) & MTL_HFINTIPPTR_PTR_MASK;
        SofDspUpdateBits(bar4, ptr + Ctx->HwDesc->IrqStsOffset, MTL_IRQ_INTEN_L_HOST_IPC_MASK, MTL_IRQ_INTEN_L_HOST_IPC_MASK);
        SofDspUpdateBits(bar4, MTL_DSP_REG_HfHIPCIE, MTL_DSP_REG_HfHIPCIE_IE_MASK, MTL_DSP_REG_HfHIPCIE_IE_MASK);
        SofDspUpdateBits(bar4, Ctx->HwDesc->HipcCtl, Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone,
                         Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone);
    }

    /* Wait for ROM status FSR_STATE_INIT_DONE (0x1) before triggering DMA */
    timeout = 1000;
    while (timeout--) {
        statusReg = SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg);
        if (FSR_TO_STATE_CODE(statusReg) == FSR_STATE_INIT_DONE ||
            FSR_TO_STATE_CODE(statusReg) == FSR_STATE_FW_ENTERED)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &(LARGE_INTEGER){.QuadPart = -10000LL}); /* 1ms */
    }

    /*
     * 5. Trigger Code Loader DMA Start:
     * Set DMA_START (RUN bit) in SD_CTL. The HD-A engine begins reading descriptors
     * and pushing firmware binary data into the DSP memory window.
     */
    writel(readl(bar0 + SOF_HDA_INTCTL) | (1 << streamIndex) | SOF_HDA_INT_GLOBAL_EN, bar0 + SOF_HDA_INTCTL);
    {
        ULONG ctlVal = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL));
        ctlVal |= SOF_HDA_SD_CTL_DMA_START | SOF_HDA_CL_DMA_SD_INT_MASK;
        WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL), ctlVal);

        KeStallExecutionProcessor(50);
        ULONG ctlAfter = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL));

        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Code loader DMA triggered on stream %u (offset 0x%x, SD_CTL=0x%08x RUN=%s), streaming firmware...\n",
                   streamIndex, sdOffset, ctlAfter, (ctlAfter & SOF_HDA_SD_CTL_DMA_START) ? "SET" : "FAILED");
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12d] Code loader DMA triggered on stream %u (offset 0x%x), SD_CTL=0x%08x (RUN=%s)\n",
            streamIndex, sdOffset, ctlAfter, (ctlAfter & SOF_HDA_SD_CTL_DMA_START) ? "SET" : "FAILED");
        SofLogBreadcrumb(bmsg);
    }

    /*
     * 6. Wait for ROM status FSR_STATE_FW_ENTERED (0x5) or IPC FW_READY:
     * Polls ROM Status register (HFDSC) for up to 4 seconds.
     */
    timeout = 4000; /* 4 seconds */
    while (timeout--) {
        statusReg = SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg);
        if (FSR_TO_STATE_CODE(statusReg) == FSR_STATE_FW_ENTERED ||
            Ctx->FwState == SOF_FW_BOOT_COMPLETE) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] *** FIRMWARE SUCCESSFULLY ENTERED (ROM Status: 0x%08x) ***\n",
                       statusReg);
            break;
        }
        if (timeout % 1000 == 0) {
            ULONG lpib = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_LPIB));
            UCHAR sdsts = READ_REGISTER_UCHAR((PUCHAR)(streamRegs + SOF_HDA_ADSP_REG_SD_STS));
            ULONG romErr = SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg);
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[12-POLL] timeout=%u: ROM_STS=0x%08x, ROM_ERR=0x%08x, LPIB=0x%x, SD_STS=0x%02x\n",
                timeout, statusReg, romErr, lpib, sdsts);
            SofLogBreadcrumb(bmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Boot poll: ROM_STS=0x%08x, ROM_ERR=0x%08x, LPIB=0x%x, SD_STS=0x%02x\n",
                       statusReg, romErr, lpib, sdsts);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &(LARGE_INTEGER){.QuadPart = -10000LL}); /* 1ms */
    }

    /* 7. Stop Code Loader DMA and disable SPIB */
    {
        ULONG ctlVal = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL));
        ctlVal &= ~SOF_HDA_SD_CTL_DMA_START;
        WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CTL), ctlVal);

        if (Ctx->SpbCapOffset) {
            PUCHAR spibBar = bar0 + Ctx->SpbCapOffset;
            PUCHAR spibAddr = spibBar + SOF_HDA_SPIB_BASE + (streamIndex * SOF_HDA_SPIB_INTERVAL) + SOF_HDA_SPIB_SPIB;
            writel(0, spibAddr);
            writel(readl(spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL) & ~(1 << streamIndex),
                   spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);
        }
    }

    /* 8. Cleanup staging DMA buffers */
    SofFreeDmaBuffer(Ctx, &bdlDma);
    SofFreeDmaBuffer(Ctx, &fwDma);

    /* Check if FW entered successfully */
    if (FSR_TO_STATE_CODE(statusReg) != FSR_STATE_FW_ENTERED &&
        Ctx->FwState != SOF_FW_BOOT_COMPLETE) {
        ULONG lpib = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_LPIB));
        UCHAR sdsts = READ_REGISTER_UCHAR((PUCHAR)(streamRegs + SOF_HDA_ADSP_REG_SD_STS));
        ULONG romErr = SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg);
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12e] Firmware boot timed out: ROM_STS=0x%08x, ROM_ERR=0x%08x, LPIB=0x%x, SD_STS=0x%02x\n",
            statusReg, romErr, lpib, sdsts);
        SofLogBreadcrumb(bmsg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Firmware boot timed out (ROM Status: 0x%08x, ERR: 0x%08x, LPIB: 0x%x, SD_STS: 0x%02x)\n",
                   statusReg, romErr, lpib, sdsts);

        /* If default failed, try community-signed binary fallback */
        if (!OptionalFwPath) {
            WCHAR communityPath[128];
            PCSTR socName = "tgl";
            if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0) socName = (Ctx->HwDesc->CoresNum == 2) ? "nvl-s" : "nvl";
            else if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) socName = "ptl";
            else if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_2_0) socName = "lnl";
            else if (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_1_0) socName = "mtl";
            else if (Ctx->HwDesc->Family == SOF_HW_INTEL_CAVS_2_0) socName = "icl";
            else if (Ctx->HwDesc->Family == SOF_HW_INTEL_CAVS_1_8) socName = "cnl";
            else if (Ctx->HwDesc->Family == SOF_HW_INTEL_CAVS_1_5) socName = "apl";

            RtlStringCbPrintfW(communityPath, sizeof(communityPath),
                               L"\\SystemRoot\\System32\\drivers\\intel\\sof\\sof-%S-community.ri",
                               socName);
            SofLogBreadcrumb("[12f] Falling back to community firmware...\n");
            return SofStartFirmwareBoot(Ctx, communityPath);
        }

        Ctx->FwState = SOF_FW_BOOT_FAILED;
        return STATUS_IO_TIMEOUT;
    }

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12e] *** FIRMWARE ENTERED (ROM Status: 0x%08x) ***\n", statusReg);
    SofLogBreadcrumb(bmsg);

    /* 9. Wait for DSP firmware initialization and SOF_IPC4_NOTIFY_FW_READY */
    SofLogBreadcrumb("[12f] Firmware entered ROM_EXT. Waiting for FW_READY notification...\n");
    timeout = 3000; /* 3 seconds */
    while (timeout--) {
        if (Ctx->FwState == SOF_FW_BOOT_COMPLETE) {
            break;
        }

        /* Poll IPC registers in case line-based interrupt was delayed */
        SofIpcDpcHandler(Ctx);

        if (Ctx->FwState == SOF_FW_BOOT_COMPLETE) {
            break;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &(LARGE_INTEGER){.QuadPart = -10000LL}); /* 1ms */
    }

    if (Ctx->FwState == SOF_FW_BOOT_COMPLETE) {
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12g] *** DSP FIRMWARE BOOT COMPLETED (FW_READY received)! ***\n");
        SofLogBreadcrumb(bmsg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Firmware boot sequence completed successfully (FW_READY).\n");

        /* Enable DSP firmware Zephyr logging if not yet initialized */
        if (!Ctx->FwLogInitialized) {
            SofIpc4EnableFwLogs(Ctx);
        }

        return STATUS_SUCCESS;
    } else {
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12g] FW_READY notification not received within timeout, but FW_ENTERED verified (ROM_STS=0x%08x)\n",
            statusReg);
        SofLogBreadcrumb(bmsg);
        Ctx->FwState = SOF_FW_BOOT_COMPLETE;
        return STATUS_SUCCESS;
    }
}

#endif /* _KERNEL_MODE */
