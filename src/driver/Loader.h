/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Loader.h
 * @brief Firmware Loader, Image Parser, and Stream DMA Staging for Sound Open Firmware.
 *
 * @details
 * Declares data structures and API functions for parsing SOF extended manifest v4 ($AE1)
 * binary images, managing physical DMA buffers via KMDF Common Buffers or contiguous physical
 * memory, configuring Intel HD-A Code Loader DMA engines, and orchestrating the DSP boot sequence
 * across Intel cAVS and ACE platforms.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_LOADER_H__
#define __SOF_LOADER_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#include <windows.h>
typedef LARGE_INTEGER PHYSICAL_ADDRESS;
#endif

#include <sof/sof_osal.h>
#include <sof/ext_manifest4.h>
#include "Hw.h"
#include "PciIds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct _SOF_DEVICE_CONTEXT *PSOF_DEVICE_CONTEXT;

/**
 * @def SOF_FW_MAX_NAME_LEN
 * @brief Maximum length of the binary name string in the firmware header.
 */
#define SOF_FW_MAX_NAME_LEN 16

/**
 * @struct _SOF_FW_IMAGE_INFO
 * @brief Parsed metadata extracted from the SOF extended manifest and binary header.
 */
typedef struct _SOF_FW_IMAGE_INFO {
    /**
     * @brief Pointer to the raw firmware file buffer loaded in memory.
     */
    PVOID   RawData;

    /**
     * @brief Total size of the raw firmware file in bytes.
     */
    ULONG   RawSize;

    /**
     * @brief Byte offset where executable payload begins (immediately following extended manifest).
     */
    ULONG   PayloadOffset;

    /**
     * @brief Size of the stripped executable firmware payload in bytes.
     */
    ULONG   PayloadSize;

    /**
     * @brief Major version component of the firmware binary.
     */
    ULONG   MajorVersion;

    /**
     * @brief Minor version component of the firmware binary.
     */
    ULONG   MinorVersion;

    /**
     * @brief Hotfix version component of the firmware binary.
     */
    ULONG   HotfixVersion;

    /**
     * @brief Build version number of the firmware binary.
     */
    ULONG   BuildVersion;

    /**
     * @brief ASCII name of the binary as declared in the manifest header.
     */
    CHAR    BinaryName[SOF_FW_MAX_NAME_LEN];

    /**
     * @brief Total count of module entries packed inside this image.
     */
    ULONG   NumModules;

    /**
     * @brief Number of initial 4KB pages to preload before triggering execution.
     */
    ULONG   PreloadPageCount;
} SOF_FW_IMAGE_INFO, *PSOF_FW_IMAGE_INFO;

/**
 * @struct _SOF_DMA_BUFFER
 * @brief DMA buffer descriptor encapsulating virtual and physical addresses for hardware transfers.
 *
 * Supports both KMDF Common Buffer allocation (with full IOMMU / VT-d support)
 * and contiguous physical memory fallback.
 */
typedef struct _SOF_DMA_BUFFER {
    /**
     * @brief Kernel virtual address accessible by the CPU.
     */
    PVOID               VirtualAddress;

    /**
     * @brief Device physical / logical address programmed into hardware DMA descriptors.
     */
    PHYSICAL_ADDRESS    PhysicalAddress;

    /**
     * @brief Total allocated size of the DMA buffer in bytes.
     */
    ULONG               Size;

#ifdef _KERNEL_MODE
    /**
     * @brief Optional Memory Descriptor List (MDL) for cache flushing.
     */
    PMDL                Mdl;

    /**
     * @brief KMDF Common Buffer object handle (NULL if using contiguous fallback).
     */
    WDFCOMMONBUFFER     CommonBuffer;
#endif
} SOF_DMA_BUFFER, *PSOF_DMA_BUFFER;

/* Image Parsing */

/**
 * @brief Validates and parses an SOF firmware binary with Extended Manifest v4.
 *
 * Inspects the magic number ($AE1 / 0x31454124), verifies manifest length boundaries,
 * calculates the stripped payload offset, and extracts version details from the base FW header.
 *
 * @param[in]  Buffer Pointer to raw firmware binary bytes in memory.
 * @param[in]  Size   Size of the buffer in bytes.
 * @param[out] Info   Pointer to caller-allocated SOF_FW_IMAGE_INFO structure to receive parsed metadata.
 * @return STATUS_SUCCESS on success; STATUS_INVALID_IMAGE_FORMAT or STATUS_INVALID_BUFFER_SIZE on validation error.
 */
NTSTATUS SofParseFirmwareImage(
    _In_reads_bytes_(Size) const void *Buffer,
    _In_ size_t Size,
    _Out_ PSOF_FW_IMAGE_INFO Info
);

/* Firmware File Loading (Kernel Mode) */
#ifdef _KERNEL_MODE

/**
 * @brief Reads a firmware binary file from the Windows filesystem into non-paged kernel pool.
 *
 * @param[in]  FilePath   Fully-qualified NT path to the firmware image (e.g. \\SystemRoot\\System32\\...).
 * @param[out] Buffer     Receives pointer to allocated memory containing file contents.
 * @param[out] Size       Receives the exact size of the loaded file in bytes.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code on failure.
 */
NTSTATUS SofLoadFirmwareFile(
    _In_ PCWSTR FilePath,
    _Out_ PVOID *Buffer,
    _Out_ PULONG Size
);

/**
 * @brief Frees a firmware buffer previously allocated by SofLoadFirmwareFile.
 *
 * @param[in] Buffer Pointer to buffer to free.
 */
VOID SofFreeFirmwareBuffer(
    _In_ PVOID Buffer
);

/**
 * @brief Allocates a DMA buffer suitable for bus-mastering hardware transfer.
 *
 * Attempts allocation via WdfCommonBufferCreate first (to leverage IOMMU / VT-d mapping).
 * If unavailable, falls back to MmAllocateContiguousNodeMemory (< 4GB boundary).
 *
 * @param[in]  Ctx    Pointer to the SOF device context.
 * @param[in]  Size   Number of bytes to allocate.
 * @param[out] DmaBuf Pointer to SOF_DMA_BUFFER structure receiving addresses and handle.
 * @return STATUS_SUCCESS on success, or STATUS_INSUFFICIENT_RESOURCES on failure.
 */
NTSTATUS SofAllocateDmaBuffer(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG Size,
    _Out_ PSOF_DMA_BUFFER DmaBuf
);

/**
 * @brief Releases a DMA buffer allocated by SofAllocateDmaBuffer.
 *
 * Deletes the WDFCOMMONBUFFER or calls MmFreeContiguousMemory depending on how it was allocated.
 *
 * @param[in]    Ctx    Pointer to the SOF device context.
 * @param[in,out] DmaBuf Pointer to SOF_DMA_BUFFER structure to free and clear.
 */
VOID SofFreeDmaBuffer(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _Inout_ PSOF_DMA_BUFFER DmaBuf
);

/**
 * @brief Configures an HD-A playback DMA stream to stage firmware into DSP memory.
 *
 * Performs:
 *  1. Verification that HD-A Global Controller is out of reset (GCTL.CRST == 1).
 *  2. Selection of Code Loader stream (first Playback stream index = NumCaptureStreams).
 *  3. Decoupling stream from HD-A link in Processing Pipe Control (PPCTL).
 *  4. Constructing a 2-entry Buffer Descriptor List (BDL) mapping the firmware payload.
 *  5. Flushing CPU caches via KeFlushIoBuffers to guarantee physical memory coherency.
 *  6. Resetting the stream engine and programming SD_CBL, SD_FORMAT (32-bit, 48kHz, 1ch),
 *     SD_LVI (index 1), and SD_BDLPL/PU.
 *  7. Programming Software Position in Buffer (SPIB) credit for decoupled transfer.
 *
 * @param[in]  Ctx            Pointer to the SOF device context.
 * @param[in]  FwDmaBuf       Pointer to DMA buffer containing stripped firmware payload.
 * @param[in]  BdlDmaBuf      Pointer to DMA buffer containing the 2-entry BDL table.
 * @param[in]  StreamTag      Stream tag number to program in SD_CTL (typically 1).
 * @param[out] OutStreamIndex Optional pointer to receive selected stream index.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofConfigureCodeLoaderDma(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PSOF_DMA_BUFFER FwDmaBuf,
    _In_ PSOF_DMA_BUFFER BdlDmaBuf,
    _In_ ULONG StreamTag,
    _Out_opt_ PULONG OutStreamIndex
);

/**
 * @brief Orchestrates full DSP firmware boot from binary file to execution.
 *
 * Execution flow:
 *  1. Loads firmware file from disk and parses manifest ($AE1).
 *  2. Allocates DMA staging memory and copies stripped payload.
 *  3. Powers down and resets DSP cores (SofPowerDownDsp).
 *  4. Programs HD-A Code Loader decoupled DMA stream and BDL descriptors.
 *  5. Powers on DSP subsystem and domain power gating on ACE (HFDSSCS, HFPWRCTL).
 *  6. Sends ROM IPC PURGE command with stream tag and waits for ROM ACK.
 *  7. Powers up DSP Primary Core (DSP2CXCTL) and sets Host CPU ownership.
 *  8. Waits for ROM status FSR_STATE_INIT_DONE (0x1).
 *  9. Triggers Code Loader DMA RUN bit.
 * 10. Polls ROM status for FSR_STATE_FW_ENTERED (0x5).
 * 11. Stops Code Loader DMA and frees staging buffers.
 * 12. Awaits SOF_IPC4_NOTIFY_FW_READY notification from booted firmware.
 *
 * @param[in,out] Ctx             Pointer to the SOF device context.
 * @param[in,opt] OptionalFwPath  Optional custom path to firmware binary; NULL for default path.
 * @return STATUS_SUCCESS on successful boot, or an NTSTATUS error code.
 */
NTSTATUS SofStartFirmwareBoot(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_opt_ PCWSTR OptionalFwPath
);

/**
 * @brief Powers down the DSP subsystem, core 0, and gates domain power on Intel ACE.
 *
 * Asserts Host CPU ownership, clears Core 0 SPA, gates DSP domain power, and powers down
 * the DSP subsystem (HFDSSCS) to place hardware into a clean, known-quiescent state.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID SofPowerDownDsp(
    _In_ PSOF_DEVICE_CONTEXT Ctx
);

#endif /* _KERNEL_MODE */

#ifdef __cplusplus
}
#endif

#endif /* __SOF_LOADER_H__ */
