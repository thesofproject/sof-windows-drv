/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Topo.h
 * @brief SOF IPC4 Topology & Audio Graph Construction Header.
 *
 * @details
 * Defines data structures, gateway types, channel maps, module identifiers, and
 * API function prototypes for constructing and controlling IPC4 audio pipelines:
 *  - Pipeline Lifecycle: Create, delete, set state (RESET, PAUSED, RUNNING).
 *  - Module Management: Copier (Host, DAI, ALH, SSP, DMIC), Level/Gain multiplier,
 *    and Up/Down Mixers.
 *  - Module Binding: Connecting source module output pins to sink module input pins.
 *  - Volume / Gain Control: Logarithmic and linear dB conversions and hardware ramps.
 * Copyright 2026 The SOF Authors
 */

#ifndef __SOF_TOPO_H__
#define __SOF_TOPO_H__

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#else
#include <windows.h>
#endif
#include <sof/sof_osal.h>
#include <sof/ipc4/header.h>
#include "Hw.h"
#include "Ipc.h"

/* Channel Map Definitions: 8 nibbles (1 per slot), unused slots set to 0xF */
#define SOF_IPC4_CH_MAP_STEREO              0xFFFFFF10  /**< Slot 0: L (0), Slot 1: R (1), Slots 2-7: Unused (0xF) */
#define SOF_IPC4_CH_MAP_MONO                0xFFFFFFF0  /**< Slot 0: Mono (0), Slots 1-7: Unused (0xF) */
#define SOF_IPC4_CH_MAP_QUAD                0xFFFF3210  /**< Slots 0-3: FL, FR, RL, RR */
#define SOF_IPC4_CH_MAP_5_1                 0xFF543210  /**< Slots 0-5: FL, FR, C, LFE, RL, RR */
#define SOF_IPC4_CH_MAP_7_1                 0x76543210  /**< Slots 0-7: FL, FR, C, LFE, RL, RR, SL, SR */

/* Gateway Node Index and Type Macros */
#define SOF_IPC4_NODE_INDEX_MASK            0xFF
#define SOF_IPC4_NODE_INDEX(x)              ((x) & SOF_IPC4_NODE_INDEX_MASK)
#define SOF_IPC4_NODE_INDEX_INTEL_SSP(x)    (((x) & 0xF) << 4)
#define SOF_IPC4_NODE_INDEX_INTEL_DMIC(x)   ((x) & 0x7)
#define SOF_IPC4_NODE_TYPE(x)               ((x) << 8)
#define SOF_IPC4_GET_NODE_TYPE(node_id)     ((node_id) >> 8)

/* Gain and Volume Definitions */
#define SOF_IPC4_GAIN_ALL_CHANNELS_MASK     0xffffffff
#define SOF_IPC4_GAIN_PARAM_ID              0

/* IPC4 Sample Types */
#define SOF_IPC4_TYPE_MSB_INTEGER           0
#define SOF_IPC4_TYPE_LSB_INTEGER           1
#define SOF_IPC4_TYPE_SIGNED_INTEGER        2
#define SOF_IPC4_TYPE_UNSIGNED_INTEGER      3
#define SOF_IPC4_TYPE_FLOAT                 4

/* Gateway Configuration Attributes */
#include <pshpack1.h>
/**
 * @struct sof_ipc4_gtw_attributes
 * @brief Hardware buffer allocation attributes for an audio gateway node.
 */
struct sof_ipc4_gtw_attributes {
    uint32_t lp_buffer_alloc : 1;       /**< 1: Allocate in Low Power (LP) SRAM; 0: HP SRAM */
    uint32_t alloc_from_reg_file : 1;   /**< 1: Allocate from register file */
    uint32_t rsvd : 30;
};

/**
 * @struct sof_copier_gateway_cfg
 * @brief Hardware configuration blob for a Copier module gateway node.
 */
struct sof_copier_gateway_cfg {
    uint32_t node_id;                   /**< Gateway node ID (type and index) */
    uint32_t dma_buffer_size;           /**< Hardware DMA buffer size in bytes */
    uint32_t config_length;             /**< Length of config_data in DWORDs */
    uint32_t config_data[1];            /**< Protocol-specific configuration payload */
};

/**
 * @struct sof_ipc4_copier_data
 * @brief Initialization payload for an IPC4 Copier module instance.
 */
struct sof_ipc4_copier_data {
    struct sof_ipc4_base_module_cfg base_config;        /**< Common base module configuration */
    struct sof_ipc4_audio_format    out_format;         /**< Output audio stream format */
    uint32_t                        copier_feature_mask;/**< Copier feature enable flags */
    struct sof_copier_gateway_cfg   gtw_cfg;            /**< Attached gateway configuration */
};

/**
 * @struct sof_ipc4_gain_params
 * @brief Parameters for an IPC4 Level Multiplier (Gain) module instance.
 */
struct sof_ipc4_gain_params {
    uint32_t channels;                  /**< Number of processed channels */
    uint32_t init_val;                  /**< Initial linear volume value (0x7FFFFFFF = 0dB) */
    uint32_t curve_type;                /**< Volume curve type: 0=Linear, 1=Logarithmic */
    uint32_t reserved;
    uint32_t curve_duration_l;          /**< Ramp duration in frames (low 32 bits) */
    uint32_t curve_duration_h;          /**< Ramp duration in frames (high 32 bits) */
};

/**
 * @struct sof_ipc4_gain_data
 * @brief Initialization payload for an IPC4 Gain module instance.
 */
struct sof_ipc4_gain_data {
    struct sof_ipc4_base_module_cfg base_config;        /**< Common base module configuration */
    struct sof_ipc4_gain_params     params;             /**< Gain ramp and initial volume parameters */
};
#include <poppack.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard IPC4 Module IDs (matching Intel ACE firmware manifest) */
#define SOF_IPC4_MOD_ID_BASEFW              2   /**< Base Firmware module index */
#define SOF_IPC4_MOD_ID_COPIER              3   /**< COPIER module index in manifest */
#define SOF_IPC4_MOD_ID_MIXER               4   /**< UPDWMIX (mixer) module index in manifest */
#define SOF_IPC4_MOD_ID_GAIN                10  /**< LVLMULT (gain) module index in manifest */

/* Module Performance & BSS Defaults */
#define SOF_IPC4_COPIER_DEFAULT_CPC         7363 /**< Manifest measured CPC for 384/384 */
#define SOF_IPC4_COPIER_DEFAULT_PAGES       1    /**< Manifest is_bytes=280 -> 1 page */
#define SOF_IPC4_GAIN_DEFAULT_CPC           1000 /**< Manifest measured CPC for LVLMULT */
#define SOF_IPC4_GAIN_DEFAULT_PAGES         1    /**< Manifest is_bytes=4096 -> 1 page */

/* Standard Gateway Node Types */
#define SOF_IPC4_GTW_HOST                   0   /**< HD/A Host DMA Output (Playback) */
#define SOF_IPC4_GTW_HOST_IN                1   /**< HD/A Host DMA Input (Capture) */
#define SOF_IPC4_GTW_SSP                    2   /**< Intel SSP / I2S Gateway */
#define SOF_IPC4_GTW_ALH                    4   /**< Audio Link Hub (SoundWire) */
#define SOF_IPC4_GTW_HDA                    8   /**< HD/A Link Output (Codec Playback) */
#define SOF_IPC4_GTW_HDA_IN                 9   /**< HD/A Link Input (Codec Capture) */
#define SOF_IPC4_GTW_DMIC                   11  /**< Digital Mic Array (PDM) */

/* Volume Constants */
#define SOF_IPC4_VOLUME_ZERO_DB             0x7fffffff  /**< Unity gain (0 dB) in Q1.31 linear representation */
#define SOF_IPC4_VOLUME_MUTE                0x00000000  /**< Complete silence / mute */
#define SOF_IPC4_VOLUME_DEFAULT_STEP        0x00100000

/* Forward declaration */
typedef struct _SOF_DEVICE_CONTEXT *PSOF_DEVICE_CONTEXT;

/**
 * @struct _SOF_AUDIO_FORMAT_PARAMS
 * @brief High-level audio format specification container.
 */
typedef struct _SOF_AUDIO_FORMAT_PARAMS {
    ULONG SamplingFrequency;    /**< Sampling rate in Hz (e.g. 48000, 96000, 192000) */
    ULONG BitDepth;             /**< Container size in bits (16, 24, 32) */
    ULONG ValidBitDepth;        /**< Valid audio bits per sample (16, 24, 32) */
    ULONG ChannelsCount;        /**< Number of audio channels (1..8) */
    ULONG InterleavingStyle;    /**< 0: Interleaved; 1: Non-interleaved */
    ULONG ChannelMap;           /**< Packed nibbles defining channel slot mapping */
    ULONG ChannelConfig;        /**< Channel configuration bitmask */
} SOF_AUDIO_FORMAT_PARAMS, *PSOF_AUDIO_FORMAT_PARAMS;

#include <pshpack1.h>
/**
 * @struct sof_ipc4_alh_mapping
 * @brief SoundWire Audio Link Hub stream mapping descriptor.
 */
struct sof_ipc4_alh_mapping {
    uint32_t alh_id;            /**< ALH channel endpoint ID */
    uint32_t channel_mask;      /**< Bitmask of mapped channels */
};

/**
 * @struct sof_ipc4_alh_configuration_blob
 * @brief Gateway configuration blob for SoundWire ALH copiers.
 */
struct sof_ipc4_alh_configuration_blob {
    struct sof_ipc4_gtw_attributes gw_attr;     /**< Gateway attributes */
    uint32_t count;                             /**< Number of ALH mapping entries */
    struct sof_ipc4_alh_mapping mapping[1];     /**< Array of ALH stream mappings */
};
#include <poppack.h>

/**
 * @struct _SOF_COPIER_PARAMS
 * @brief Parameters for constructing a Copier module initialization payload.
 */
typedef struct _SOF_COPIER_PARAMS {
    ULONG StreamTag;            /**< HD-A Stream Tag (1..15) */
    ULONG NodeType;             /**< Gateway type (SOF_IPC4_GTW_*) */
    ULONG NodeIndex;            /**< Stream ID - 1 for Host, DAI index for DAI */
    ULONG BufferSize;           /**< Buffer size in bytes (typically 4ms) */
    ULONG AlhStreamId;          /**< ALH Stream ID for SoundWire (Gateway 4) */
    SOF_AUDIO_FORMAT_PARAMS AudioFormat; /**< Associated audio format */
} SOF_COPIER_PARAMS, *PSOF_COPIER_PARAMS;

/**
 * @struct _SOF_PIPELINE_DESC
 * @brief Audio pipeline descriptor tracking state in driver memory.
 */
typedef struct _SOF_PIPELINE_DESC {
    ULONG PipelineId;           /**< Unique pipeline ID */
    ULONG Priority;             /**< Scheduling priority (0 highest, 7 lowest) */
    ULONG MemUsagePages;        /**< Number of 4KB memory pages reserved in DSP */
    ULONG CoreId;               /**< Target DSP Core ID */
    ULONG LpMode;               /**< Low-power mode (0 or 1) */
    ULONG State;                /**< Current state (SOF_IPC4_PIPE_*) */
} SOF_PIPELINE_DESC, *PSOF_PIPELINE_DESC;

/* Format Builder Helpers */

/**
 * @brief Converts driver audio format struct to IPC4 firmware format descriptor.
 *
 * @param[in]  InParams Pointer to SOF_AUDIO_FORMAT_PARAMS.
 * @param[out] OutFmt   Pointer to firmware sof_ipc4_audio_format structure.
 */
VOID SofIpc4BuildAudioFormat(
    _In_ const PSOF_AUDIO_FORMAT_PARAMS InParams,
    _Out_ struct sof_ipc4_audio_format *OutFmt
);

/**
 * @brief Populates default stereo PCM audio format settings.
 *
 * @param[in]  SamplingFrequency Sample rate in Hz (e.g. 48000).
 * @param[in]  BitDepth          Bit depth (16 or 32).
 * @param[out] OutParams         Receives populated format structure.
 */
VOID SofIpc4BuildDefaultStereoFormat(
    _In_ ULONG SamplingFrequency,
    _In_ ULONG BitDepth,
    _Out_ PSOF_AUDIO_FORMAT_PARAMS OutParams
);

/* Module Configuration Builders */

/**
 * @brief Builds the initialization payload blob for an IPC4 Copier module instance.
 *
 * @param[in]  Params       Pointer to copier parameters.
 * @param[out] CopierBuffer Buffer receiving the serialized configuration blob.
 * @param[in]  MaxBuffer    Size of CopierBuffer in bytes.
 * @param[out] OutLength    Receives actual number of bytes written.
 * @return STATUS_SUCCESS on success, or STATUS_BUFFER_TOO_SMALL.
 */
NTSTATUS SofIpc4BuildCopierConfig(
    _In_ const PSOF_COPIER_PARAMS Params,
    _Out_writes_bytes_(MaxBuffer) PVOID CopierBuffer,
    _In_ ULONG MaxBuffer,
    _Out_ PULONG OutLength
);

/**
 * @brief Builds the initialization payload blob for an IPC4 Gain module instance.
 *
 * @param[in]  Channels   Number of audio channels.
 * @param[in]  InitVal    Initial linear gain (0x7FFFFFFF = 0dB).
 * @param[out] GainBuffer Buffer receiving the serialized configuration blob.
 * @param[in]  MaxBuffer  Size of GainBuffer in bytes.
 * @param[out] OutLength  Receives actual number of bytes written.
 * @return STATUS_SUCCESS on success, or STATUS_BUFFER_TOO_SMALL.
 */
NTSTATUS SofIpc4BuildGainConfig(
    _In_ ULONG Channels,
    _In_ ULONG InitVal,
    _Out_writes_bytes_(MaxBuffer) PVOID GainBuffer,
    _In_ ULONG MaxBuffer,
    _Out_ PULONG OutLength
);

/* IPC4 Pipeline Management */

/**
 * @brief Sends SOF_IPC4_GLB_CREATE_PIPELINE message to instantiate a new DSP audio pipeline.
 *
 * @param[in] Ctx           Pointer to SOF device context.
 * @param[in] PipelineId    Unique pipeline identifier.
 * @param[in] Priority      Priority level (0 highest, 7 lowest).
 * @param[in] MemUsagePages Number of 4KB pages to reserve.
 * @param[in] CoreId        Target DSP core (typically 0).
 * @param[in] LpMode        1 for LP SRAM; 0 for HP SRAM.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofIpc4CreatePipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG Priority,
    _In_ ULONG MemUsagePages,
    _In_ ULONG CoreId,
    _In_ ULONG LpMode
);

/**
 * @brief Sends SOF_IPC4_GLB_DELETE_PIPELINE message to destroy an existing DSP audio pipeline.
 *
 * @param[in] Ctx        Pointer to SOF device context.
 * @param[in] PipelineId Pipeline ID to delete.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4DeletePipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId
);

/**
 * @brief Sends SOF_IPC4_GLB_SET_PIPELINE_STATE to transition pipeline state (RUN, PAUSE, RESET).
 *
 * @param[in] Ctx        Pointer to SOF device context.
 * @param[in] PipelineId Pipeline ID.
 * @param[in] State      Target state (SOF_IPC4_PIPE_RESET, PAUSED, RUNNING).
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4SetPipelineState(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG State
);

/* IPC4 Module Instantiation & Binding */

/**
 * @brief Sends SOF_IPC4_GLB_INIT_INSTANCE to instantiate a module inside an audio pipeline.
 *
 * @param[in] Ctx         Pointer to SOF device context.
 * @param[in] ModuleId    Module type identifier (COPIER, GAIN, MIXER).
 * @param[in] InstanceId  Unique instance ID within the module type.
 * @param[in] PipelineId  Associated pipeline ID.
 * @param[in] CoreId      Target DSP core ID.
 * @param[in] Payload     Pointer to module configuration blob.
 * @param[in] PayloadSize Size of configuration blob in bytes.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4InitModuleInstance(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG ModuleId,
    _In_ ULONG InstanceId,
    _In_ ULONG PipelineId,
    _In_ ULONG CoreId,
    _In_reads_bytes_(PayloadSize) const void *Payload,
    _In_ ULONG PayloadSize
);

/**
 * @brief Connects source module output pin to destination module input pin via SOF_IPC4_GLB_BIND.
 *
 * @param[in] Ctx           Pointer to SOF device context.
 * @param[in] SrcModuleId   Source module type ID.
 * @param[in] SrcInstanceId Source module instance ID.
 * @param[in] SrcQueue      Source output queue / pin index.
 * @param[in] DstModuleId   Destination module type ID.
 * @param[in] DstInstanceId Destination module instance ID.
 * @param[in] DstQueue      Destination input queue / pin index.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4BindModules(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG SrcModuleId,
    _In_ ULONG SrcInstanceId,
    _In_ ULONG SrcQueue,
    _In_ ULONG DstModuleId,
    _In_ ULONG DstInstanceId,
    _In_ ULONG DstQueue
);

/**
 * @brief Disconnects modules via SOF_IPC4_GLB_UNBIND.
 *
 * @param[in] Ctx           Pointer to SOF device context.
 * @param[in] SrcModuleId   Source module type ID.
 * @param[in] SrcInstanceId Source module instance ID.
 * @param[in] SrcQueue      Source output queue index.
 * @param[in] DstModuleId   Destination module type ID.
 * @param[in] DstInstanceId Destination module instance ID.
 * @param[in] DstQueue      Destination input queue index.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4UnbindModules(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG SrcModuleId,
    _In_ ULONG SrcInstanceId,
    _In_ ULONG SrcQueue,
    _In_ ULONG DstModuleId,
    _In_ ULONG DstInstanceId,
    _In_ ULONG DstQueue
);

/* Volume and Audio Control Definitions */
#define SOF_VOLUME_MIN_DB_UNITS             (-96 * 65536)       /**< -96 dB (in 1/65536 dB units) */
#define SOF_VOLUME_MAX_DB_UNITS             (0)                 /**< 0 dB */
#define SOF_VOLUME_STEP_DB_UNITS            (32768)             /**< 0.5 dB step */
#define SOF_VOLUME_DEFAULT_RAMP_FRAMES      (960)               /**< 20 ms ramp at 48 kHz */
#define SOF_IPC4_GAIN_CURVE_TYPE_LINEAR     0
#define SOF_IPC4_GAIN_CURVE_TYPE_LOG        1

/* Volume Conversion Helpers (Zero float, integer interpolation) */

/**
 * @brief Converts Windows 1/65536 dB units to linear Q1.31 gain.
 *
 * @param[in] VolumeDb Volume level in 1/65536 dB units (clamped to [-96 dB, 0 dB]).
 * @return 32-bit linear gain value (0x7FFFFFFF = 0 dB, 0x00000000 = mute).
 */
ULONG SofWindowsDbToSofGain(
    _In_ LONG VolumeDb
);

/**
 * @brief Converts linear Q1.31 gain to Windows 1/65536 dB units.
 *
 * @param[in] GainLinear 32-bit linear gain value.
 * @return Volume in 1/65536 dB units.
 */
LONG SofGainToWindowsDb(
    _In_ ULONG GainLinear
);

/* Volume and Mute Control */

/**
 * @brief Sends an IPC4 volume parameter to a Gain module instance.
 *
 * @param[in,opt] Ctx            Pointer to SOF device context.
 * @param[in]     GainModuleId   Gain module type ID.
 * @param[in]     GainInstanceId Gain module instance ID.
 * @param[in]     ChannelMask    Bitmask of channels to update.
 * @param[in]     Volume         Linear volume value.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4SetVolume(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainModuleId,
    _In_ ULONG GainInstanceId,
    _In_ ULONG ChannelMask,
    _In_ ULONG Volume
);

/**
 * @brief Sets volume in dB units for a specific channel on a Gain module.
 *
 * @param[in,opt] Ctx            Pointer to SOF device context.
 * @param[in]     GainInstanceId Gain module instance ID.
 * @param[in]     Channel        Channel index (0, 1, or SOF_IPC4_GAIN_ALL_CHANNELS_MASK).
 * @param[in]     VolumeDb       Volume level in 1/65536 dB units.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4SetVolumeLevel(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainInstanceId,
    _In_ ULONG Channel,
    _In_ LONG VolumeDb
);

/**
 * @brief Mutes or unmutes a Gain module instance.
 *
 * @param[in,opt] Ctx            Pointer to SOF device context.
 * @param[in]     GainInstanceId Gain module instance ID.
 * @param[in]     Channel        Channel index.
 * @param[in]     Mute           TRUE to mute; FALSE to unmute.
 * @param[in]     TargetVolumeDb Target volume in dB when unmuting.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4SetMute(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainInstanceId,
    _In_ ULONG Channel,
    _In_ BOOLEAN Mute,
    _In_ LONG TargetVolumeDb
);

/**
 * @brief Programs a volume ramp curve on a Gain module instance.
 *
 * @param[in,opt] Ctx            Pointer to SOF device context.
 * @param[in]     GainInstanceId Gain module instance ID.
 * @param[in]     Channel        Channel index.
 * @param[in]     VolumeLinear   Target linear volume.
 * @param[in]     CurveType      0 for Linear; 1 for Logarithmic.
 * @param[in]     DurationFrames Duration in audio frames.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4SetVolumeCurve(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainInstanceId,
    _In_ ULONG Channel,
    _In_ ULONG VolumeLinear,
    _In_ ULONG CurveType,
    _In_ ULONG DurationFrames
);

/* Endpoint Definitions */

/**
 * @enum _SOF_ENDPOINT_TYPE
 * @brief Enumerates supported physical audio endpoints.
 */
typedef enum _SOF_ENDPOINT_TYPE {
    SOF_ENDPOINT_HDMI_1 = 0,    /**< iDisp1 Playback (Link Index 1) */
    SOF_ENDPOINT_HDMI_2,        /**< iDisp2 Playback (Link Index 2) */
    SOF_ENDPOINT_HDMI_3,        /**< iDisp3 Playback (Link Index 3) */
    SOF_ENDPOINT_I2S_SSP2,      /**< SSP2 I2S Playback/Capture (Port 2) */
    SOF_ENDPOINT_HDA_ANALOG,    /**< HDA Codec Analog Playback/Capture (Link 0) */
    SOF_ENDPOINT_DMIC_PDM,      /**< PDM Digital Microphone Array */
    SOF_ENDPOINT_SDW_RENDER,    /**< SoundWire Render (ALH Gateway 4) */
    SOF_ENDPOINT_SDW_CAPTURE,   /**< SoundWire Capture (ALH Gateway 4) */
    SOF_ENDPOINT_USB_OFFLOAD,   /**< Intel USB Audio Offload Link (UAOL) */
    SOF_ENDPOINT_COUNT
} SOF_ENDPOINT_TYPE;

/* Instance ID Allocator */

/**
 * @brief Allocates a unique module instance ID from the device context tracking bitmap.
 *
 * @param[in,opt] Ctx           Pointer to SOF device context.
 * @param[in]     ModuleId      Module type ID.
 * @param[out]    OutInstanceId Receives allocated instance ID.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4AllocInstanceId(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG ModuleId,
    _Out_ PULONG OutInstanceId
);

/**
 * @brief Frees a previously allocated module instance ID.
 *
 * @param[in,opt] Ctx        Pointer to SOF device context.
 * @param[in]     ModuleId   Module type ID.
 * @param[in]     InstanceId Instance ID to release.
 */
VOID SofIpc4FreeInstanceId(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG ModuleId,
    _In_ ULONG InstanceId
);

/* High-Level Pipeline Assembly */

/**
 * @brief Creates and binds an Analog Playback pipeline (Host Copier -> Gain -> HD-A DAI Copier).
 *
 * @param[in]  Ctx         Pointer to SOF device context.
 * @param[in]  PipelineId  Pipeline identifier.
 * @param[in]  StreamTag   HD-A Stream Tag.
 * @param[in]  Fmt         Audio format parameters.
 * @param[out] OutHostInst Receives Host Copier instance ID.
 * @param[out] OutGainInst Receives Gain instance ID.
 * @param[out] OutDaiInst  Receives DAI Copier instance ID.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4CreatePlaybackPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
);

/**
 * @brief Creates and binds an Analog Capture pipeline (HD-A DAI Copier -> Gain -> Host Copier).
 *
 * @param[in]  Ctx         Pointer to SOF device context.
 * @param[in]  PipelineId  Pipeline identifier.
 * @param[in]  StreamTag   HD-A Stream Tag.
 * @param[in]  Fmt         Audio format parameters.
 * @param[out] OutHostInst Receives Host Copier instance ID.
 * @param[out] OutGainInst Receives Gain instance ID.
 * @param[out] OutDaiInst  Receives DAI Copier instance ID.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4CreateCapturePipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
);

/**
 * @brief Creates and binds an Intel Display Audio HDMI pipeline.
 *
 * @param[in]  Ctx          Pointer to SOF device context.
 * @param[in]  PipelineId   Pipeline identifier.
 * @param[in]  StreamTag    HD-A Stream Tag.
 * @param[in]  HdmiLinkIndex HDMI link index (1..3).
 * @param[in]  Fmt          Audio format parameters.
 * @param[out] OutHostInst  Receives Host Copier instance ID.
 * @param[out] OutGainInst  Receives Gain instance ID.
 * @param[out] OutDaiInst   Receives DAI Copier instance ID.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4CreateHdmiPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ ULONG HdmiLinkIndex,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
);

/**
 * @brief Creates and binds an I2S SSP audio pipeline.
 *
 * @param[in]  Ctx         Pointer to SOF device context.
 * @param[in]  PipelineId  Pipeline identifier.
 * @param[in]  StreamTag   HD-A Stream Tag.
 * @param[in]  SspPort     SSP port index (e.g. 2).
 * @param[in]  IsCapture   TRUE for capture; FALSE for playback.
 * @param[in]  Fmt         Audio format parameters.
 * @param[out] OutHostInst Receives Host Copier instance ID.
 * @param[out] OutGainInst Receives Gain instance ID.
 * @param[out] OutDaiInst  Receives DAI Copier instance ID.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4CreateSspPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ ULONG SspPort,
    _In_ BOOLEAN IsCapture,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
);

/**
 * @brief Creates and binds a PDM Digital Microphone Array (DMIC) capture pipeline.
 *
 * @param[in]  Ctx         Pointer to SOF device context.
 * @param[in]  PipelineId  Pipeline identifier.
 * @param[in]  StreamTag   HD-A Stream Tag.
 * @param[in]  Fmt         Audio format parameters.
 * @param[out] OutHostInst Receives Host Copier instance ID.
 * @param[out] OutGainInst Receives Gain instance ID.
 * @param[out] OutDaiInst  Receives DAI Copier instance ID.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4CreateDmicPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
);

/**
 * @brief Creates and binds a SoundWire Audio Link Hub (ALH) pipeline.
 *
 * @param[in]  Ctx         Pointer to SOF device context.
 * @param[in]  PipelineId  Pipeline identifier.
 * @param[in]  StreamTag   HD-A Stream Tag.
 * @param[in]  LinkIndex   SoundWire link index.
 * @param[in]  IsCapture   TRUE for capture; FALSE for playback.
 * @param[in]  Fmt         Audio format parameters.
 * @param[out] OutHostInst Receives Host Copier instance ID.
 * @param[out] OutGainInst Receives Gain instance ID.
 * @param[out] OutDaiInst  Receives DAI Copier instance ID.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofIpc4CreateAlhPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ ULONG LinkIndex,
    _In_ BOOLEAN IsCapture,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
);

#ifdef __cplusplus
}
#endif

#endif /* __SOF_TOPO_H__ */
