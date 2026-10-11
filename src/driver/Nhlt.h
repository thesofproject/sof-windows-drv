/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Nhlt.h
 * @brief Intel Non-HD Audio Configuration Table (NHLT) ACPI Parser Header.
 *
 * @details
 * Declares ACPI table structures and parser routines for dynamically querying and
 * decoding the Intel NHLT ACPI table:
 *  - DMIC Array Topology: Microphone count (2ch stereo, 4ch quad), geometry, and formats.
 *  - I2S / SSP Endpoints: Codec identifiers and port configurations.
 *  - SoundWire Endpoints: Master link index, device type, and peripheral characteristics.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_NHLT_H__
#define __SOF_NHLT_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <portcls.h>
#else
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#endif
#include <sof/sof_osal.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def NHLT_ACPI_SIGNATURE
 * @brief 4-byte ACPI table signature 'NHLT' (0x544C484E in little-endian).
 */
#define NHLT_ACPI_SIGNATURE             0x544C484E

/**
 * @def ACPI_PROVIDER_SIGNATURE
 * @brief Provider signature 'ACPI' (0x41435049 in little-endian).
 */
#define ACPI_PROVIDER_SIGNATURE         0x41435049

/* NHLT Link Types */
#define NHLT_LINK_HDA                   0   /**< HD-A Link */
#define NHLT_LINK_DSP                   1   /**< DSP Interconnect */
#define NHLT_LINK_DMIC                  2   /**< PDM Digital Microphone Array */
#define NHLT_LINK_SSP                   3   /**< Intel I2S / SSP Port */
#define NHLT_LINK_SLIMBUS               4   /**< SLIMbus */
#define NHLT_LINK_SDW                   5   /**< MIPI SoundWire Link */

/* NHLT Endpoint Directions */
#define NHLT_DIRECTION_RENDER           0   /**< Playback / Output */
#define NHLT_DIRECTION_CAPTURE          1   /**< Capture / Input */

/* Standard DMIC Configurations */
#define NHLT_DMIC_2CH                   2   /**< 2-channel stereo mic array */
#define NHLT_DMIC_4CH                   4   /**< 4-channel quad mic array */

#include <pshpack1.h>

/**
 * @struct _ACPI_NHLT_HEADER
 * @brief Standard 36-byte ACPI Table Header for NHLT.
 */
typedef struct _ACPI_NHLT_HEADER {
    ULONG   Signature;          /**< 'NHLT' (0x544C484E) */
    ULONG   Length;             /**< Length of table in bytes including header */
    UCHAR   Revision;           /**< Table Revision */
    UCHAR   Checksum;           /**< Table Checksum (entire table sums to 0) */
    UCHAR   OemId[6];           /**< OEM Identifier string */
    UCHAR   OemTableId[8];      /**< OEM Table Identifier string */
    ULONG   OemRevision;        /**< OEM Revision Number */
    ULONG   CreatorId;          /**< ASL Compiler Vendor ID */
    ULONG   CreatorRevision;    /**< ASL Compiler Revision */
} ACPI_NHLT_HEADER, *PACPI_NHLT_HEADER;

/**
 * @struct _NHLT_SPECIFIC_CONFIG
 * @brief Vendor-Specific Configuration Blob (VSD) attached to NHLT descriptors.
 */
typedef struct _NHLT_SPECIFIC_CONFIG {
    ULONG   CapabilitiesSize;   /**< Size of Capabilities array in bytes */
    UCHAR   Capabilities[1];    /**< Vendor-specific configuration payload */
} NHLT_SPECIFIC_CONFIG, *PNHLT_SPECIFIC_CONFIG;

/**
 * @struct _NHLT_FORMAT_CONFIG
 * @brief Audio format specification and format-specific configuration blob.
 */
typedef struct _NHLT_FORMAT_CONFIG {
    WAVEFORMATEXTENSIBLE Format;             /**< Standard Windows extended wave format */
    ULONG                SpecificConfigSize; /**< Size of SpecificConfig in bytes */
    UCHAR                SpecificConfig[1];  /**< Format-specific configuration payload */
} NHLT_FORMAT_CONFIG, *PNHLT_FORMAT_CONFIG;

/**
 * @struct _NHLT_FORMATS_CONFIG
 * @brief Header preceding the array of supported audio format descriptors.
 */
typedef struct _NHLT_FORMATS_CONFIG {
    UCHAR   FormatsCount;       /**< Total number of format descriptors following */
    /* Flexible array of NHLT_FORMAT_CONFIG items */
} NHLT_FORMATS_CONFIG, *PNHLT_FORMATS_CONFIG;

/**
 * @struct _NHLT_ENDPOINT_DESCRIPTOR
 * @brief Descriptor describing a single non-HD audio endpoint in the NHLT table.
 */
typedef struct _NHLT_ENDPOINT_DESCRIPTOR {
    ULONG   Length;             /**< Total size of this endpoint structure in bytes */
    UCHAR   LinkType;           /**< Link Type (NHLT_LINK_DMIC, SSP, SDW, etc.) */
    UCHAR   InstanceId;         /**< Controller link instance index */
    USHORT  HwVendorId;         /**< Hardware Vendor ID */
    USHORT  HwDeviceId;         /**< Hardware Device / Part ID */
    USHORT  HwRevisionId;       /**< Hardware Revision ID */
    ULONG   HwSubsystemId;      /**< Hardware Subsystem ID */
    UCHAR   DeviceType;         /**< Endpoint Device Type */
    UCHAR   Direction;          /**< 0: Render, 1: Capture */
    UCHAR   VirtualBusId;       /**< Virtual bus identifier */
    /* Trailing SpecificConfig and FormatsConfig */
} NHLT_ENDPOINT_DESCRIPTOR, *PNHLT_ENDPOINT_DESCRIPTOR;

#include <poppack.h>

/**
 * @struct _SOF_NHLT_DMIC_INFO
 * @brief Parsed Digital Microphone Array capabilities extracted from NHLT.
 */
typedef struct _SOF_NHLT_DMIC_INFO {
    BOOLEAN Present;            /**< TRUE if DMIC array is defined in NHLT */
    ULONG   ChannelCount;       /**< Channel count (2 or 4) */
    ULONG   SampleRate;         /**< Primary sample rate in Hz (e.g. 48000) */
    ULONG   BitDepth;           /**< Container bit depth (16, 24, 32) */
    ULONG   ArrayType;          /**< Microphone geometry type */
} SOF_NHLT_DMIC_INFO, *PSOF_NHLT_DMIC_INFO;

/**
 * @def SOF_MAX_SDW_ENDPOINTS
 * @brief Maximum number of SoundWire endpoints parsed from NHLT.
 */
#define SOF_MAX_SDW_ENDPOINTS           8

/**
 * @struct _SOF_NHLT_SDW_ENDPOINT
 * @brief Parsed SoundWire endpoint configuration extracted from NHLT.
 */
typedef struct _SOF_NHLT_SDW_ENDPOINT {
    BOOLEAN             Present;        /**< TRUE if endpoint is valid */
    UCHAR               LinkIndex;      /**< Controller master link instance (0..3) */
    UCHAR               Direction;      /**< 0: Render, 1: Capture */
    USHORT              VendorId;       /**< MIPI Vendor ID */
    USHORT              DeviceId;       /**< Peripheral Device / Part ID */
    UCHAR               DeviceType;     /**< SDCA Device Type */
    ULONG               SampleRate;     /**< Sample rate in Hz */
    ULONG               ChannelCount;   /**< Channel count */
    ULONG               BitDepth;       /**< Bit depth */
} SOF_NHLT_SDW_ENDPOINT, *PSOF_NHLT_SDW_ENDPOINT;

/**
 * @struct _SOF_NHLT_INFO
 * @brief Summary container holding all parsed NHLT audio hardware capabilities.
 */
typedef struct _SOF_NHLT_INFO {
    BOOLEAN             IsValid;                /**< TRUE if NHLT was successfully parsed */
    ULONG               TotalEndpoints;         /**< Total count of endpoint descriptors */
    ULONG               DmicEndpointsCount;     /**< Count of discovered DMIC endpoints */
    ULONG               SspEndpointsCount;      /**< Count of discovered SSP endpoints */
    ULONG               SdwEndpointsCount;      /**< Count of discovered SoundWire endpoints */

    /* Primary DMIC configuration */
    SOF_NHLT_DMIC_INFO  Dmic;                   /**< Primary DMIC array geometry */

    /* Off-Chip Codec Info (if present on SSP or SoundWire) */
    BOOLEAN             ExternalCodecPresent;   /**< TRUE if external audio codec detected */
    USHORT              CodecVendorId;          /**< Codec Vendor ID */
    USHORT              CodecDeviceId;          /**< Codec Device ID */
    UCHAR               CodecLinkType;          /**< Link type where codec resides */
    UCHAR               CodecInstanceId;        /**< Link instance index */

    /* SoundWire Endpoints Discovered from NHLT */
    SOF_NHLT_SDW_ENDPOINT SdwEndpoints[SOF_MAX_SDW_ENDPOINTS]; /**< Array of discovered SoundWire endpoints */

} SOF_NHLT_INFO, *PSOF_NHLT_INFO;

/* NHLT Parser Functions */

/**
 * @brief Parses an in-memory ACPI NHLT table and extracts all audio endpoint metadata.
 *
 * @param[in]  NhltBuffer Pointer to raw ACPI NHLT table bytes.
 * @param[in]  NhltSize   Total size of NhltBuffer in bytes.
 * @param[out] Info       Pointer to SOF_NHLT_INFO structure receiving parsed metadata.
 * @return STATUS_SUCCESS on success, or an NTSTATUS validation error code.
 */
NTSTATUS SofNhltParse(
    _In_reads_bytes_(NhltSize) const void *NhltBuffer,
    _In_ size_t NhltSize,
    _Out_ PSOF_NHLT_INFO Info
);

/* Kernel ACPI Firmware Query */

/**
 * @brief Queries the ACPI subsystem to locate and read the system NHLT table.
 *
 * @param[out] NhltBufferOut Receives pointer to allocated memory containing table.
 * @param[out] NhltSizeOut   Receives size of table in bytes.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofNhltQueryAcpiTable(
    _Out_ PVOID *NhltBufferOut,
    _Out_ PULONG NhltSizeOut
);

/**
 * @brief Frees an NHLT table buffer previously queried by SofNhltQueryAcpiTable.
 *
 * @param[in] NhltBuffer Pointer to buffer to free.
 */
VOID SofNhltFreeAcpiTable(
    _In_ PVOID NhltBuffer
);

#ifdef __cplusplus
}
#endif

#endif /* __SOF_NHLT_H__ */
