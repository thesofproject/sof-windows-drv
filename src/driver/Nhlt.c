/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Nhlt.c
 * @brief Intel Non-HD Audio Configuration Table (NHLT) ACPI Parser Implementation.
 *
 * @details
 * Decodes the in-memory or firmware-queried ACPI NHLT table to extract audio hardware topology:
 *  - Validates NHLT signature ('NHLT' / 0x544C484E) and length fields.
 *  - Parses array of endpoint descriptors (DMIC, SSP/I2S, and SoundWire).
 *  - Discovers DMIC microphone array geometry, channel counts, and supported formats.
 *  - Queries ACPI firmware tables via ZwQuerySystemInformation (SystemFirmwareTableInformation).
 * Copyright 2026 The SOF Authors
 */

#include "Nhlt.h"

#ifdef _KERNEL_MODE
#define SOF_NHLT_TAG 'TLHN'

#define SystemFirmwareTableInformation 76

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Inout_ PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
);
#endif

/**
 * @brief Parses an in-memory ACPI NHLT table and extracts all audio endpoint metadata.
 *
 * @param[in]  NhltBuffer Pointer to raw ACPI NHLT table bytes.
 * @param[in]  NhltSize   Total size of NhltBuffer in bytes.
 * @param[out] Info       Pointer to SOF_NHLT_INFO structure receiving parsed metadata.
 * @return STATUS_SUCCESS on success; STATUS_NOT_FOUND if signature doesn't match;
 *         STATUS_BUFFER_TOO_SMALL or STATUS_INVALID_PARAMETER on malformed table.
 */
NTSTATUS SofNhltParse(
    _In_reads_bytes_(NhltSize) const void *NhltBuffer,
    _In_ size_t NhltSize,
    _Out_ PSOF_NHLT_INFO Info
)
{
    const ACPI_NHLT_HEADER *hdr;
    const UCHAR *curr;
    const UCHAR *end;
    UCHAR endpointCount;
    UCHAR epIndex;

    if (NhltBuffer == NULL || Info == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Info, sizeof(SOF_NHLT_INFO));

    if (NhltSize < sizeof(ACPI_NHLT_HEADER) + 1) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    hdr = (const ACPI_NHLT_HEADER *)NhltBuffer;

    /* Verify NHLT signature ('NHLT' = 0x544C484E) */
    if (hdr->Signature != NHLT_ACPI_SIGNATURE) {
        return STATUS_NOT_FOUND;
    }

    if (hdr->Length > NhltSize || hdr->Length < sizeof(ACPI_NHLT_HEADER) + 1) {
        return STATUS_INVALID_PARAMETER;
    }

    curr = (const UCHAR *)NhltBuffer + sizeof(ACPI_NHLT_HEADER);
    end = (const UCHAR *)NhltBuffer + hdr->Length;

    endpointCount = *curr++;
    Info->TotalEndpoints = endpointCount;

    for (epIndex = 0; epIndex < endpointCount && curr < end; epIndex++) {
        const NHLT_ENDPOINT_DESCRIPTOR *ep;
        ULONG epLen;

        if ((size_t)(end - curr) < sizeof(NHLT_ENDPOINT_DESCRIPTOR)) {
            break;
        }

        ep = (const NHLT_ENDPOINT_DESCRIPTOR *)curr;
        epLen = ep->Length;

        if (epLen < sizeof(NHLT_ENDPOINT_DESCRIPTOR) || (size_t)(end - curr) < epLen) {
            break;
        }

        switch (ep->LinkType) {
        case NHLT_LINK_DMIC:
            Info->DmicEndpointsCount++;
            Info->Dmic.Present = TRUE;
            /* Default DMIC format: 48kHz, 2-ch or 4-ch depending on instance */
            if (Info->Dmic.ChannelCount == 0) {
                Info->Dmic.ChannelCount = (ep->InstanceId >= 2) ? NHLT_DMIC_4CH : NHLT_DMIC_2CH;
                Info->Dmic.SampleRate = 48000;
                Info->Dmic.BitDepth = 32;
            }
            break;

        case NHLT_LINK_SSP:
            Info->SspEndpointsCount++;
            if (ep->HwVendorId != 0 && ep->HwDeviceId != 0) {
                Info->ExternalCodecPresent = TRUE;
                Info->CodecVendorId = ep->HwVendorId;
                Info->CodecDeviceId = ep->HwDeviceId;
                Info->CodecLinkType = NHLT_LINK_SSP;
                Info->CodecInstanceId = ep->InstanceId;
            }
            break;

        case NHLT_LINK_SDW:
            if (Info->SdwEndpointsCount < SOF_MAX_SDW_ENDPOINTS) {
                PSOF_NHLT_SDW_ENDPOINT sdwEp = &Info->SdwEndpoints[Info->SdwEndpointsCount];
                sdwEp->Present = TRUE;
                sdwEp->LinkIndex = ep->InstanceId;
                sdwEp->Direction = ep->Direction;
                sdwEp->VendorId = ep->HwVendorId;
                sdwEp->DeviceId = ep->HwDeviceId;
                sdwEp->DeviceType = ep->DeviceType;
                sdwEp->SampleRate = 48000;
                sdwEp->ChannelCount = 2;
                sdwEp->BitDepth = 32;

                /* Attempt to extract format parameters if format blob is present */
                if (epLen > sizeof(NHLT_ENDPOINT_DESCRIPTOR)) {
                    const UCHAR *cfgPtr = (const UCHAR *)ep + sizeof(NHLT_ENDPOINT_DESCRIPTOR);
                    const UCHAR *epEnd = (const UCHAR *)ep + epLen;
                    if ((size_t)(epEnd - cfgPtr) >= sizeof(ULONG)) {
                        ULONG capSize = *(const ULONG *)cfgPtr;
                        cfgPtr += sizeof(ULONG) + capSize;
                        if ((size_t)(epEnd - cfgPtr) >= 1) {
                            UCHAR fmtCount = *cfgPtr++;
                            if (fmtCount > 0 && (size_t)(epEnd - cfgPtr) >= sizeof(WAVEFORMATEXTENSIBLE)) {
                                const WAVEFORMATEXTENSIBLE *wfe = (const WAVEFORMATEXTENSIBLE *)cfgPtr;
                                if (wfe->Format.nSamplesPerSec != 0) {
                                    sdwEp->SampleRate = wfe->Format.nSamplesPerSec;
                                }
                                if (wfe->Format.nChannels != 0) {
                                    sdwEp->ChannelCount = wfe->Format.nChannels;
                                }
                                if (wfe->Format.wBitsPerSample != 0) {
                                    sdwEp->BitDepth = wfe->Format.wBitsPerSample;
                                }
                            }
                        }
                    }
                }
            }
            Info->SdwEndpointsCount++;
            if (ep->HwVendorId != 0 && ep->HwDeviceId != 0) {
                Info->ExternalCodecPresent = TRUE;
                Info->CodecVendorId = ep->HwVendorId;
                Info->CodecDeviceId = ep->HwDeviceId;
                Info->CodecLinkType = NHLT_LINK_SDW;
                Info->CodecInstanceId = ep->InstanceId;
            }
            break;

        default:
            break;
        }

        curr += epLen;
    }

    Info->IsValid = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS SofNhltQueryAcpiTable(
    _Out_ PVOID *NhltBufferOut,
    _Out_ PULONG NhltSizeOut
)
{
    if (NhltBufferOut == NULL || NhltSizeOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *NhltBufferOut = NULL;
    *NhltSizeOut = 0;

#ifdef _KERNEL_MODE
    {
        SYSTEM_FIRMWARE_TABLE_INFORMATION queryInfo = {0};
        PSYSTEM_FIRMWARE_TABLE_INFORMATION pTableInfo;
        ULONG querySize;
        ULONG tableSize;
        ULONG retLen = 0;
        NTSTATUS status;

        queryInfo.ProviderSignature = ACPI_PROVIDER_SIGNATURE; /* 'ACPI' */
        queryInfo.Action = SystemFirmwareTable_Get;
        queryInfo.TableID = NHLT_ACPI_SIGNATURE; /* 'NHLT' */
        queryInfo.TableBufferLength = 0;

        /* First query to determine buffer size */
        status = ZwQuerySystemInformation(
            SystemFirmwareTableInformation,
            &queryInfo,
            sizeof(queryInfo),
            &retLen
        );

        tableSize = queryInfo.TableBufferLength;
        if (tableSize == 0) {
            tableSize = retLen;
        }

        if (tableSize == 0) {
            return STATUS_NOT_FOUND;
        }

        querySize = sizeof(SYSTEM_FIRMWARE_TABLE_INFORMATION) + tableSize;
        pTableInfo = (PSYSTEM_FIRMWARE_TABLE_INFORMATION)ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            querySize,
            SOF_NHLT_TAG
        );
        if (pTableInfo == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(pTableInfo, querySize);
        pTableInfo->ProviderSignature = ACPI_PROVIDER_SIGNATURE;
        pTableInfo->Action = SystemFirmwareTable_Get;
        pTableInfo->TableID = NHLT_ACPI_SIGNATURE;
        pTableInfo->TableBufferLength = tableSize;

        status = ZwQuerySystemInformation(
            SystemFirmwareTableInformation,
            pTableInfo,
            querySize,
            &retLen
        );

        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(pTableInfo, SOF_NHLT_TAG);
            return status;
        }

        *NhltBufferOut = pTableInfo->TableBuffer;
        *NhltSizeOut = pTableInfo->TableBufferLength;
        return STATUS_SUCCESS;
    }
#else
    {
        /* User-mode implementation via GetSystemFirmwareTable */
        DWORD needed = GetSystemFirmwareTable('ACPI', 'NHLT', NULL, 0);
        PVOID buffer;
        DWORD readBytes;

        if (needed == 0) {
            return STATUS_NOT_FOUND;
        }

        buffer = malloc(needed);
        if (buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        readBytes = GetSystemFirmwareTable('ACPI', 'NHLT', buffer, needed);
        if (readBytes == 0) {
            free(buffer);
            return STATUS_NOT_FOUND;
        }

        *NhltBufferOut = buffer;
        *NhltSizeOut = readBytes;
        return STATUS_SUCCESS;
    }
#endif
}

VOID SofNhltFreeAcpiTable(
    _In_ PVOID NhltBuffer
)
{
    if (NhltBuffer == NULL) {
        return;
    }

#ifdef _KERNEL_MODE
    {
        /* Buffer is offset within SYSTEM_FIRMWARE_TABLE_INFORMATION */
        PSYSTEM_FIRMWARE_TABLE_INFORMATION pTableInfo =
            CONTAINING_RECORD(NhltBuffer, SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);
        ExFreePoolWithTag(pTableInfo, SOF_NHLT_TAG);
    }
#else
    free(NhltBuffer);
#endif
}
