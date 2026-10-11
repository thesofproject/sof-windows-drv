/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Topo.c
 * @brief SOF IPC4 Topology & Audio Graph Construction Implementation.
 *
 * @details
 * Implements low-level pipeline creation, module configuration blobs, instance
 * allocation, audio format conversion, and module binding across Intel cAVS/ACE
 * DSP firmware architectures:
 *  - Format Builders: Computes IPC4 fmt_cfg and channel mapping.
 *  - Copier Config: Serializes gateway configuration for Host DMA, ALH, SSP, DMIC, HDA.
 *  - Gain Config: Serializes volume ramp parameters and initial gain levels.
 *  - Pipeline Control: Issues SOF_IPC4_GLB_CREATE_PIPELINE, DELETE_PIPELINE, and SET_PIPELINE_STATE.
 *  - Module Lifecycle: Issues SOF_IPC4_GLB_INIT_INSTANCE, BIND, and UNBIND.
 *  - Volume / Mute DDI: Converts Windows 1/65536 dB units to Q1.31 linear values.
 * Copyright 2026 The SOF Authors
 */

#include "Topo.h"
#ifdef _KERNEL_MODE
#include "Device.h"
#include <ntstrsafe.h>
void SofLogBreadcrumb(const char* text);
#else
#include <stdio.h>
#define SofLogBreadcrumb(x) ((void)0)
#define RtlStringCbPrintfA(buf, sz, fmt, ...) snprintf(buf, sz, fmt, __VA_ARGS__)
#endif

/**
 * @brief Serializes driver audio format struct into IPC4 firmware format descriptor.
 *
 * @param[in]  InParams Pointer to SOF_AUDIO_FORMAT_PARAMS.
 * @param[out] OutFmt   Pointer to firmware sof_ipc4_audio_format structure.
 */
VOID SofIpc4BuildAudioFormat(
    _In_ const PSOF_AUDIO_FORMAT_PARAMS InParams,
    _Out_ struct sof_ipc4_audio_format *OutFmt
)
{
    RtlZeroMemory(OutFmt, sizeof(*OutFmt));
    OutFmt->sampling_frequency = InParams->SamplingFrequency;
    OutFmt->bit_depth = InParams->BitDepth;
    OutFmt->ch_map = InParams->ChannelMap;
    OutFmt->ch_cfg = InParams->ChannelConfig;
    OutFmt->interleaving_style = InParams->InterleavingStyle;

    /* fmt_cfg packing:
     * Bits 0-7:   Channels count
     * Bits 8-15:  Valid bit depth
     * Bits 16-23: Sample type (SOF_IPC4_TYPE_LSB_INTEGER = 1 for standard PCM LE)
     */
    OutFmt->fmt_cfg = (InParams->ChannelsCount & 0xFF) |
                      ((InParams->ValidBitDepth & 0xFF) << 8) |
                      (SOF_IPC4_TYPE_LSB_INTEGER << 16);
}

VOID SofIpc4BuildDefaultStereoFormat(
    _In_ ULONG SamplingFrequency,
    _In_ ULONG BitDepth,
    _Out_ PSOF_AUDIO_FORMAT_PARAMS OutParams
)
{
    RtlZeroMemory(OutParams, sizeof(*OutParams));
    OutParams->SamplingFrequency = SamplingFrequency;
    OutParams->BitDepth = BitDepth;
    OutParams->ValidBitDepth = BitDepth;
    OutParams->ChannelsCount = 2;
    OutParams->InterleavingStyle = SOF_IPC4_CHANNELS_INTERLEAVED;
    OutParams->ChannelMap = SOF_IPC4_CH_MAP_STEREO; /* 0xFFFFFF10: Left = 0, Right = 1, Unused = 0xF */
    OutParams->ChannelConfig = SOF_IPC4_CHANNEL_CONFIG_STEREO;
}

NTSTATUS SofIpc4BuildCopierConfig(
    _In_ const PSOF_COPIER_PARAMS Params,
    _Out_writes_bytes_(MaxBuffer) PVOID CopierBuffer,
    _In_ ULONG MaxBuffer,
    _Out_ PULONG OutLength
)
{
    struct sof_ipc4_copier_data *copier;
    struct sof_ipc4_gtw_attributes *gtwAttr;
    ULONG totalSize;
    ULONG chunkBytes;

    /* struct sof_ipc4_copier_data already contains config_data[1] for gtw attributes */
    totalSize = sizeof(struct sof_ipc4_copier_data);
    if (MaxBuffer < totalSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(CopierBuffer, totalSize);
    copier = (struct sof_ipc4_copier_data *)CopierBuffer;

    /* Build audio format */
    SofIpc4BuildAudioFormat(&Params->AudioFormat, &copier->base_config.audio_fmt);
    copier->out_format = copier->base_config.audio_fmt;

    /* Calculate 1ms chunk size for ibs/obs (e.g. 384 bytes for 48kHz stereo 32-bit) */
    chunkBytes = (Params->AudioFormat.SamplingFrequency *
                  Params->AudioFormat.ChannelsCount *
                  (Params->AudioFormat.BitDepth / 8)) / 1000;
    if (chunkBytes == 0) {
        chunkBytes = 384;
    }
    copier->base_config.ibs = chunkBytes;
    copier->base_config.obs = chunkBytes;
    copier->base_config.cpc = SOF_IPC4_COPIER_DEFAULT_CPC; /* 7363 from manifest */
    copier->base_config.is_pages = SOF_IPC4_COPIER_DEFAULT_PAGES; /* 1 page BSS */
    copier->copier_feature_mask = 0;

    /* Gateway configuration */
    copier->gtw_cfg.node_id = SOF_IPC4_NODE_TYPE(Params->NodeType) |
                              SOF_IPC4_NODE_INDEX(Params->NodeIndex);
    copier->gtw_cfg.dma_buffer_size = 4 * chunkBytes; /* Minimum 4ms DMA buffer */

    if (Params->NodeType == SOF_IPC4_GTW_ALH) {
        struct sof_ipc4_alh_configuration_blob *alhBlob;
        totalSize = sizeof(struct sof_ipc4_copier_data) + sizeof(struct sof_ipc4_alh_configuration_blob) - sizeof(uint32_t);
        if (MaxBuffer < totalSize) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        copier->gtw_cfg.config_length = sizeof(struct sof_ipc4_alh_configuration_blob) / sizeof(uint32_t);
        alhBlob = (struct sof_ipc4_alh_configuration_blob *)copier->gtw_cfg.config_data;
        alhBlob->gw_attr.lp_buffer_alloc = 0;
        alhBlob->gw_attr.alloc_from_reg_file = 0;
        alhBlob->gw_attr.rsvd = 0;
        alhBlob->count = 1;
        alhBlob->mapping[0].alh_id = Params->AlhStreamId;
        alhBlob->mapping[0].channel_mask = (1 << Params->AudioFormat.ChannelsCount) - 1;
    } else {
        copier->gtw_cfg.config_length = sizeof(struct sof_ipc4_gtw_attributes) / sizeof(uint32_t);

        /* Gateway attributes trailing in config_data */
        gtwAttr = (struct sof_ipc4_gtw_attributes *)copier->gtw_cfg.config_data;
        gtwAttr->lp_buffer_alloc = 0;
        gtwAttr->alloc_from_reg_file = 0;
        gtwAttr->rsvd = 0;
    }

    *OutLength = totalSize;
    return STATUS_SUCCESS;
}

NTSTATUS SofIpc4BuildGainConfig(
    _In_ ULONG Channels,
    _In_ ULONG InitVal,
    _Out_writes_bytes_(MaxBuffer) PVOID GainBuffer,
    _In_ ULONG MaxBuffer,
    _Out_ PULONG OutLength
)
{
    struct sof_ipc4_gain_data *gain;
    ULONG totalSize;
    SOF_AUDIO_FORMAT_PARAMS fmtParams;
    ULONG chunkBytes;

    totalSize = sizeof(struct sof_ipc4_gain_data);
    if (MaxBuffer < totalSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(GainBuffer, totalSize);
    gain = (struct sof_ipc4_gain_data *)GainBuffer;

    SofIpc4BuildDefaultStereoFormat(48000, 32, &fmtParams);
    fmtParams.ChannelsCount = Channels;
    SofIpc4BuildAudioFormat(&fmtParams, &gain->base_config.audio_fmt);

    chunkBytes = (fmtParams.SamplingFrequency *
                  fmtParams.ChannelsCount *
                  (fmtParams.BitDepth / 8)) / 1000;
    if (chunkBytes == 0) {
        chunkBytes = 384;
    }
    gain->base_config.ibs = chunkBytes;
    gain->base_config.obs = chunkBytes;
    gain->base_config.cpc = SOF_IPC4_GAIN_DEFAULT_CPC; /* 1000 from manifest */
    gain->base_config.is_pages = SOF_IPC4_GAIN_DEFAULT_PAGES; /* 1 page BSS */

    gain->params.channels = SOF_IPC4_GAIN_ALL_CHANNELS_MASK;
    gain->params.init_val = InitVal;
    gain->params.curve_type = 0; /* Linear curve */
    gain->params.curve_duration_l = 0;
    gain->params.curve_duration_h = 0;

    *OutLength = totalSize;
    return STATUS_SUCCESS;
}

NTSTATUS SofIpc4CreatePipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG Priority,
    _In_ ULONG MemUsagePages,
    _In_ ULONG CoreId,
    _In_ ULONG LpMode
)
{
    SOF_IPC_MESSAGE msg = {0};

    msg.Primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_GLB_CREATE_PIPELINE) |
                  SOF_IPC4_GLB_PIPE_INSTANCE_ID(PipelineId) |
                  SOF_IPC4_GLB_PIPE_PRIORITY(Priority) |
                  SOF_IPC4_GLB_PIPE_MEM_SIZE(MemUsagePages);

    msg.Extension = SOF_IPC4_GLB_PIPE_EXT_LP(LpMode) |
                    SOF_IPC4_GLB_PIPE_EXT_CORE_ID(CoreId);

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        return SofIpcSendMessage(Ctx, &msg, NULL, 500);
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS SofIpc4DeletePipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId
)
{
    SOF_IPC_MESSAGE msg = {0};

    msg.Primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_GLB_DELETE_PIPELINE) |
                  SOF_IPC4_GLB_PIPE_INSTANCE_ID(PipelineId);

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        return SofIpcSendMessage(Ctx, &msg, NULL, 500);
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS SofIpc4SetPipelineState(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG State
)
{
    SOF_IPC_MESSAGE msg = {0};

    msg.Primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_GLB_SET_PIPELINE_STATE) |
                  SOF_IPC4_GLB_PIPE_STATE_ID(PipelineId) |
                  SOF_IPC4_GLB_PIPE_STATE(State);

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        return SofIpcSendMessage(Ctx, &msg, NULL, 500);
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

#ifndef _KERNEL_MODE
static ULONG s_CopierMask = 0;
static ULONG s_GainMask = 0;
#endif

NTSTATUS SofIpc4AllocInstanceId(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG ModuleId,
    _Out_ PULONG OutInstanceId
)
{
    ULONG *mask = NULL;
    ULONG maxInstances = 32;

    if (OutInstanceId == NULL) return STATUS_INVALID_PARAMETER;

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        if (ModuleId == SOF_IPC4_MOD_ID_COPIER) {
            mask = &Ctx->CopierInstanceMask;
            maxInstances = 32;
        } else if (ModuleId == SOF_IPC4_MOD_ID_GAIN) {
            mask = &Ctx->GainInstanceMask;
            maxInstances = 40;
        }
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
    if (ModuleId == SOF_IPC4_MOD_ID_COPIER) {
        mask = &s_CopierMask;
    } else if (ModuleId == SOF_IPC4_MOD_ID_GAIN) {
        mask = &s_GainMask;
    }
#endif

    if (mask != NULL) {
        for (ULONG i = 0; i < min(maxInstances, 32); i++) {
            if (!(*mask & (1UL << i))) {
                *mask |= (1UL << i);
                *OutInstanceId = i;
                return STATUS_SUCCESS;
            }
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *OutInstanceId = 0;
    return STATUS_SUCCESS;
}

VOID SofIpc4FreeInstanceId(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG ModuleId,
    _In_ ULONG InstanceId
)
{
    ULONG *mask = NULL;
#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        if (ModuleId == SOF_IPC4_MOD_ID_COPIER) {
            mask = &Ctx->CopierInstanceMask;
        } else if (ModuleId == SOF_IPC4_MOD_ID_GAIN) {
            mask = &Ctx->GainInstanceMask;
        }
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
    if (ModuleId == SOF_IPC4_MOD_ID_COPIER) {
        mask = &s_CopierMask;
    } else if (ModuleId == SOF_IPC4_MOD_ID_GAIN) {
        mask = &s_GainMask;
    }
#endif

    if (mask != NULL && InstanceId < 32) {
        *mask &= ~(1UL << InstanceId);
    }
}

NTSTATUS SofIpc4InitModuleInstance(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG ModuleId,
    _In_ ULONG InstanceId,
    _In_ ULONG PipelineId,
    _In_ ULONG CoreId,
    _In_reads_bytes_(PayloadSize) const void *Payload,
    _In_ ULONG PayloadSize
)
{
    SOF_IPC_MESSAGE msg = {0};

    ULONG paramDwords = (PayloadSize + 3) / sizeof(uint32_t);

    msg.Primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_MOD_INIT_INSTANCE) |
                  SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG) |
                  SOF_IPC4_MOD_INSTANCE(InstanceId) |
                  SOF_IPC4_MOD_ID(ModuleId);

    msg.Extension = SOF_IPC4_MOD_EXT_PARAM_SIZE(paramDwords) |
                    SOF_IPC4_MOD_EXT_PPL_ID(PipelineId) |
                    SOF_IPC4_MOD_EXT_CORE_ID(CoreId);

    msg.Payload = (PVOID)Payload;
    msg.PayloadSize = PayloadSize;

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        return SofIpcSendMessage(Ctx, &msg, NULL, 500);
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS SofIpc4BindModules(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG SrcModuleId,
    _In_ ULONG SrcInstanceId,
    _In_ ULONG SrcQueue,
    _In_ ULONG DstModuleId,
    _In_ ULONG DstInstanceId,
    _In_ ULONG DstQueue
)
{
    SOF_IPC_MESSAGE msg = {0};

    msg.Primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_MOD_BIND) |
                  SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG) |
                  SOF_IPC4_MOD_INSTANCE(SrcInstanceId) |
                  SOF_IPC4_MOD_ID(SrcModuleId);

    msg.Extension = SOF_IPC4_MOD_EXT_DST_MOD_ID(DstModuleId) |
                    SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE(DstInstanceId) |
                    SOF_IPC4_MOD_EXT_DST_MOD_QUEUE_ID(DstQueue) |
                    SOF_IPC4_MOD_EXT_SRC_MOD_QUEUE_ID(SrcQueue);

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        return SofIpcSendMessage(Ctx, &msg, NULL, 500);
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS SofIpc4UnbindModules(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG SrcModuleId,
    _In_ ULONG SrcInstanceId,
    _In_ ULONG SrcQueue,
    _In_ ULONG DstModuleId,
    _In_ ULONG DstInstanceId,
    _In_ ULONG DstQueue
)
{
    SOF_IPC_MESSAGE msg = {0};

    msg.Primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_MOD_UNBIND) |
                  SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG) |
                  SOF_IPC4_MOD_INSTANCE(SrcInstanceId) |
                  SOF_IPC4_MOD_ID(SrcModuleId);

    msg.Extension = SOF_IPC4_MOD_EXT_DST_MOD_ID(DstModuleId) |
                    SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE(DstInstanceId) |
                    SOF_IPC4_MOD_EXT_DST_MOD_QUEUE_ID(DstQueue) |
                    SOF_IPC4_MOD_EXT_SRC_MOD_QUEUE_ID(SrcQueue);

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        return SofIpcSendMessage(Ctx, &msg, NULL, 500);
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

/*
 * Volume dB Conversion Table: 0 dB to -96 dB attenuation.
 * Index = attenuation in dB (0..96).
 * Value = Q1.31 linear gain (0x7FFFFFFF = 0 dB unity gain).
 */
static const ULONG SofVolumeDbTable[97] = {
    0x7FFFFFFF, 0x721482BF, 0x65AC8C2E, 0x5A9DF7AB, 0x50C335D3, 0x47FACCF0, 0x4026E73C, 0x392CED8E,
    0x32F52CFF, 0x2D6A866F, 0x287A26C4, 0x241346F6, 0x2026F30F, 0x1CA7D768, 0x198A1357, 0x16C310E3,
    0x144960C5, 0x12149A60, 0x101D3F2D, 0x0E5CA14C, 0x0CCCCCCD, 0x0B68737A, 0x0A2ADAD1, 0x090FCBF7,
    0x08138562, 0x0732AE18, 0x066A4A53, 0x05B7B15B, 0x05188480, 0x048AA70B, 0x040C3714, 0x039B8719,
    0x0337184E, 0x02DD958A, 0x028DCEBC, 0x0246B4E4, 0x0207567A, 0x01CEDC3D, 0x019C8651, 0x016FA9BB,
    0x0147AE14, 0x01240B8C, 0x01044915, 0x00E7FACC, 0x00CEC08A, 0x00B8449C, 0x00A43AA2, 0x00925E89,
    0x008273A6, 0x007443E8, 0x00679F1C, 0x005C5A4F, 0x00524F3B, 0x00495BC1, 0x00416179, 0x003A454A,
    0x0033EF0C, 0x002E4939, 0x002940A2, 0x0024C42C, 0x0020C49C, 0x001D345B, 0x001A074F, 0x001732AE,
    0x0014ACDB, 0x00126D43, 0x00106C43, 0x000EA30E, 0x000D0B91, 0x000BA064, 0x000A5CB6, 0x00093C3B,
    0x00083B20, 0x000755FA, 0x000689BF, 0x0005D3BB, 0x00053181, 0x0004A0EC, 0x00042010, 0x0003AD38,
    0x000346DC, 0x0002EBA3, 0x00029A55, 0x000251DE, 0x00021149, 0x0001D7BA, 0x0001A46D, 0x000176B5,
    0x00014DF5, 0x000129A4, 0x00010945, 0x0000EC6C, 0x0000D2B6, 0x0000BBCC, 0x0000A760, 0x0000952C,
    0x000084F3
};

ULONG SofWindowsDbToSofGain(
    _In_ LONG VolumeDb
)
{
    LONG negV;
    ULONG d;
    ULONG rem;
    ULONG valD, valNext;
    ULONGLONG diff;

    if (VolumeDb >= 0) {
        return SOF_IPC4_VOLUME_ZERO_DB;
    }
    if (VolumeDb <= SOF_VOLUME_MIN_DB_UNITS || VolumeDb == (LONG)0x80000000) {
        return SOF_IPC4_VOLUME_MUTE;
    }

    negV = -VolumeDb;
    d = (ULONG)(negV >> 16);
    rem = (ULONG)(negV & 0xFFFF);

    if (d >= 96) {
        return SOF_IPC4_VOLUME_MUTE;
    }

    valD = SofVolumeDbTable[d];
    valNext = SofVolumeDbTable[d + 1];
    diff = (ULONGLONG)(valD - valNext) * rem;

    return (ULONG)(valD - (diff >> 16));
}

LONG SofGainToWindowsDb(
    _In_ ULONG GainLinear
)
{
    ULONG d;

    if (GainLinear >= SOF_IPC4_VOLUME_ZERO_DB) {
        return 0;
    }
    if (GainLinear <= SofVolumeDbTable[96]) {
        return SOF_VOLUME_MIN_DB_UNITS;
    }

    for (d = 0; d < 96; d++) {
        if (SofVolumeDbTable[d] >= GainLinear && GainLinear >= SofVolumeDbTable[d + 1]) {
            ULONG diff = SofVolumeDbTable[d] - SofVolumeDbTable[d + 1];
            ULONG rem = 0;
            if (diff > 0) {
                ULONGLONG numer = ((ULONGLONG)(SofVolumeDbTable[d] - GainLinear)) << 16;
                rem = (ULONG)(numer / diff);
            }
            return -(LONG)((d << 16) + rem);
        }
    }

    return SOF_VOLUME_MIN_DB_UNITS;
}

NTSTATUS SofIpc4SetVolumeCurve(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainInstanceId,
    _In_ ULONG Channel,
    _In_ ULONG VolumeLinear,
    _In_ ULONG CurveType,
    _In_ ULONG DurationFrames
)
{
    SOF_IPC_MESSAGE msg = {0};
    struct sof_ipc4_gain_params params = {0};

    params.channels = Channel;
    params.init_val = VolumeLinear;
    params.curve_type = CurveType;
    params.curve_duration_l = DurationFrames;
    params.curve_duration_h = 0;

    msg.Primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_MOD_LARGE_CONFIG_SET) |
                  SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG) |
                  SOF_IPC4_MOD_INSTANCE(GainInstanceId) |
                  SOF_IPC4_MOD_ID(SOF_IPC4_MOD_ID_GAIN);

    msg.Extension = SOF_IPC4_MOD_EXT_MSG_SIZE(sizeof(params)) |
                    SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_GAIN_PARAM_ID) |
                    SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK(1) |
                    SOF_IPC4_MOD_EXT_MSG_LAST_BLOCK(1);

    msg.Payload = &params;
    msg.PayloadSize = sizeof(params);

#ifdef _KERNEL_MODE
    if (Ctx != NULL) {
        return SofIpcSendMessage(Ctx, &msg, NULL, 500);
    }
#else
    UNREFERENCED_PARAMETER(Ctx);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS SofIpc4SetVolumeLevel(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainInstanceId,
    _In_ ULONG Channel,
    _In_ LONG VolumeDb
)
{
    ULONG gain = SofWindowsDbToSofGain(VolumeDb);
    return SofIpc4SetVolumeCurve(Ctx, GainInstanceId, Channel, gain,
                                 SOF_IPC4_GAIN_CURVE_TYPE_LINEAR,
                                 SOF_VOLUME_DEFAULT_RAMP_FRAMES);
}

NTSTATUS SofIpc4SetMute(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainInstanceId,
    _In_ ULONG Channel,
    _In_ BOOLEAN Mute,
    _In_ LONG TargetVolumeDb
)
{
    ULONG gain;

    if (Mute) {
        gain = SOF_IPC4_VOLUME_MUTE;
    } else {
        gain = SofWindowsDbToSofGain(TargetVolumeDb);
    }

    return SofIpc4SetVolumeCurve(Ctx, GainInstanceId, Channel, gain,
                                 SOF_IPC4_GAIN_CURVE_TYPE_LINEAR,
                                 SOF_VOLUME_DEFAULT_RAMP_FRAMES);
}

NTSTATUS SofIpc4SetVolume(
    _In_opt_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG GainModuleId,
    _In_ ULONG GainInstanceId,
    _In_ ULONG ChannelMask,
    _In_ ULONG Volume
)
{
    UNREFERENCED_PARAMETER(GainModuleId);
    return SofIpc4SetVolumeCurve(Ctx, GainInstanceId, ChannelMask, Volume, 0, 0);
}

NTSTATUS SofIpc4CreatePlaybackPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
)
{
    NTSTATUS status;
    UCHAR copierBuffer[256];
    ULONG copierLen;
    UCHAR gainBuffer[128];
    ULONG gainLen;
    SOF_COPIER_PARAMS hostCopierParams = {0};
    SOF_COPIER_PARAMS daiCopierParams = {0};
    ULONG hostInst = 0, gainInst = 0, daiInst = 0;

    if (OutHostInst) *OutHostInst = 0xFFFFFFFF;
    if (OutGainInst) *OutGainInst = 0xFFFFFFFF;
    if (OutDaiInst) *OutDaiInst = 0xFFFFFFFF;

    /* 1. Create pipeline with priority 0, 16 memory pages, Core 0 */
    status = SofIpc4CreatePipeline(Ctx, PipelineId, 0, 16, 0, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[15-P1] CreatePipeline(ID %u) failed: 0x%08x\n", PipelineId, status);
        SofLogBreadcrumb(bmsg);
        return status;
    }
    SofLogBreadcrumb("[15-P1] CreatePipeline succeeded\n");

    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &hostInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, &gainInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &daiInst);

    /* 2. Instantiate Host Copier */
    hostCopierParams.StreamTag = StreamTag;
    hostCopierParams.NodeType = SOF_IPC4_GTW_HOST;
    hostCopierParams.NodeIndex = StreamTag - 1;
    hostCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[15-P2] Init Host Copier failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[15-P2] Init Host Copier succeeded\n");

    /* 3. Instantiate Gain Module with 0 dB */
    status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[15-P3] Init Gain failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[15-P3] Init Gain succeeded\n");

    /* 4. Instantiate HD-A Codec DAI Copier */
    daiCopierParams.StreamTag = StreamTag;
    daiCopierParams.NodeType = SOF_IPC4_GTW_HDA;
    daiCopierParams.NodeIndex = 0; /* Codec link 0 */
    daiCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&daiCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, daiInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[15-P4] Init DAI Copier failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[15-P4] Init DAI Copier succeeded\n");

    /* 5. Bind Host Copier -> Gain */
    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[15-P5] Bind Copier->Gain failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[15-P5] Bind Copier->Gain succeeded\n");

    /* 6. Bind Gain -> Codec DAI Copier */
    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, daiInst, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[15-P6] Bind Gain->DAI failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[15-P6] Bind Gain->DAI succeeded\n");

    /* 7. Set pipeline state to PAUSED (ready for stream start) */
    status = SofIpc4SetPipelineState(Ctx, PipelineId, SOF_IPC4_PIPE_PAUSED);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[15-P7] SetPipelineState PAUSED failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[15-P7] SetPipelineState PAUSED succeeded\n");

    if (OutHostInst) *OutHostInst = hostInst;
    if (OutGainInst) *OutGainInst = gainInst;
    if (OutDaiInst) *OutDaiInst = daiInst;
    return status;

cleanup:
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
    SofIpc4DeletePipeline(Ctx, PipelineId);
    return status;
}

NTSTATUS SofIpc4CreateCapturePipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
)
{
    NTSTATUS status;
    UCHAR copierBuffer[256];
    ULONG copierLen;
    UCHAR gainBuffer[128];
    ULONG gainLen;
    SOF_COPIER_PARAMS daiCopierParams = {0};
    SOF_COPIER_PARAMS hostCopierParams = {0};
    ULONG hostInst = 0, gainInst = 0, daiInst = 0;

    if (OutHostInst) *OutHostInst = 0xFFFFFFFF;
    if (OutGainInst) *OutGainInst = 0xFFFFFFFF;
    if (OutDaiInst) *OutDaiInst = 0xFFFFFFFF;

    /* 1. Create pipeline with priority 0, 16 memory pages, Core 0 */
    status = SofIpc4CreatePipeline(Ctx, PipelineId, 0, 16, 0, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[16-C1] CreatePipeline(ID %u) failed: 0x%08x\n", PipelineId, status);
        SofLogBreadcrumb(bmsg);
        return status;
    }
    SofLogBreadcrumb("[16-C1] CreatePipeline succeeded\n");

    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &daiInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, &gainInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &hostInst);

    /* 2. Instantiate DAI Copier (HD-A Link Capture) */
    daiCopierParams.StreamTag = StreamTag;
    daiCopierParams.NodeType = SOF_IPC4_GTW_HDA_IN;
    daiCopierParams.NodeIndex = 0;
    daiCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&daiCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, daiInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[16-C2] Init DAI Copier failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[16-C2] Init DAI Copier succeeded\n");

    /* 3. Instantiate Gain Module with 0 dB */
    status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[16-C3] Init Gain failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[16-C3] Init Gain succeeded\n");

    /* 4. Instantiate Host Copier */
    hostCopierParams.StreamTag = StreamTag;
    hostCopierParams.NodeType = SOF_IPC4_GTW_HOST_IN;
    hostCopierParams.NodeIndex = StreamTag - 1;
    hostCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[16-C4] Init Host Copier failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[16-C4] Init Host Copier succeeded\n");

    /* 5. Bind DAI Copier -> Gain */
    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, daiInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[16-C5] Bind DAI->Gain failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[16-C5] Bind DAI->Gain succeeded\n");

    /* 6. Bind Gain -> Host Copier */
    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, hostInst, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[16-C6] Bind Gain->Host failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[16-C6] Bind Gain->Host succeeded\n");

    /* 7. Set pipeline state to PAUSED */
    status = SofIpc4SetPipelineState(Ctx, PipelineId, SOF_IPC4_PIPE_PAUSED);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[16-C7] SetPipelineState PAUSED failed: 0x%08x\n", status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }
    SofLogBreadcrumb("[16-C7] SetPipelineState PAUSED succeeded\n");

    if (OutHostInst) *OutHostInst = hostInst;
    if (OutGainInst) *OutGainInst = gainInst;
    if (OutDaiInst) *OutDaiInst = daiInst;
    return status;

cleanup:
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, daiInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4DeletePipeline(Ctx, PipelineId);
    return status;
}

NTSTATUS SofIpc4CreateHdmiPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ ULONG HdmiLinkIndex,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
)
{
    NTSTATUS status;
    UCHAR copierBuffer[256];
    ULONG copierLen;
    UCHAR gainBuffer[128];
    ULONG gainLen;
    SOF_COPIER_PARAMS hostCopierParams = {0};
    SOF_COPIER_PARAMS hdmiCopierParams = {0};
    ULONG hostInst = 0, gainInst = 0, hdmiInst = 0;

    if (OutHostInst) *OutHostInst = 0xFFFFFFFF;
    if (OutGainInst) *OutGainInst = 0xFFFFFFFF;
    if (OutDaiInst) *OutDaiInst = 0xFFFFFFFF;

    /* 1. Create pipeline with priority 0, 16 memory pages, Core 0 */
    status = SofIpc4CreatePipeline(Ctx, PipelineId, 0, 16, 0, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDMI%u] CreatePipeline(ID %u) failed: 0x%08x\n", HdmiLinkIndex, PipelineId, status);
        SofLogBreadcrumb(bmsg);
        return status;
    }

    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &hostInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, &gainInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &hdmiInst);

    /* 2. Instantiate Host Copier */
    hostCopierParams.StreamTag = StreamTag;
    hostCopierParams.NodeType = SOF_IPC4_GTW_HOST;
    hostCopierParams.NodeIndex = StreamTag - 1;
    hostCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDMI%u] Init Host Copier failed: 0x%08x\n", HdmiLinkIndex, status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }

    /* 3. Instantiate Gain Module with 0 dB */
    status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDMI%u] Init Gain failed: 0x%08x\n", HdmiLinkIndex, status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }

    /* 4. Instantiate HDMI DAI Copier (Node Type 8, Node Index = HdmiLinkIndex) */
    hdmiCopierParams.StreamTag = StreamTag;
    hdmiCopierParams.NodeType = SOF_IPC4_GTW_HDA;
    hdmiCopierParams.NodeIndex = HdmiLinkIndex;
    hdmiCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&hdmiCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hdmiInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDMI%u] Init DAI Copier failed: 0x%08x\n", HdmiLinkIndex, status);
        SofLogBreadcrumb(bmsg);
        goto cleanup;
    }

    /* 5. Bind Host Copier -> Gain */
    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
    if (!NT_SUCCESS(status)) goto cleanup;

    /* 6. Bind Gain -> HDMI DAI Copier */
    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, hdmiInst, 0);
    if (!NT_SUCCESS(status)) goto cleanup;

    /* 7. Set pipeline state to PAUSED */
    status = SofIpc4SetPipelineState(Ctx, PipelineId, SOF_IPC4_PIPE_PAUSED);
    if (NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[HDMI%u] Pipeline %u created and set to PAUSED (Host=%u, Gain=%u, DAI=%u)\n",
            HdmiLinkIndex, PipelineId, hostInst, gainInst, hdmiInst);
        SofLogBreadcrumb(bmsg);
    }

    if (OutHostInst) *OutHostInst = hostInst;
    if (OutGainInst) *OutGainInst = gainInst;
    if (OutDaiInst) *OutDaiInst = hdmiInst;
    return status;

cleanup:
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, hdmiInst);
    SofIpc4DeletePipeline(Ctx, PipelineId);
    return status;
}

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
)
{
    NTSTATUS status;
    UCHAR copierBuffer[256];
    ULONG copierLen;
    UCHAR gainBuffer[128];
    ULONG gainLen;
    SOF_COPIER_PARAMS hostCopierParams = {0};
    SOF_COPIER_PARAMS sspCopierParams = {0};
    ULONG hostInst = 0, gainInst = 0, sspInst = 0;

    if (OutHostInst) *OutHostInst = 0xFFFFFFFF;
    if (OutGainInst) *OutGainInst = 0xFFFFFFFF;
    if (OutDaiInst) *OutDaiInst = 0xFFFFFFFF;

    /* 1. Create pipeline with priority 0, 16 memory pages, Core 0 */
    status = SofIpc4CreatePipeline(Ctx, PipelineId, 0, 16, 0, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[SSP%u] CreatePipeline(ID %u) failed: 0x%08x\n", SspPort, PipelineId, status);
        SofLogBreadcrumb(bmsg);
        return status;
    }

    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &hostInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, &gainInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &sspInst);

    if (!IsCapture) {
        /* Playback: Host Copier -> Gain -> SSP DAI Copier */
        hostCopierParams.StreamTag = StreamTag;
        hostCopierParams.NodeType = SOF_IPC4_GTW_HOST;
        hostCopierParams.NodeIndex = StreamTag - 1;
        hostCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        sspCopierParams.StreamTag = StreamTag;
        sspCopierParams.NodeType = SOF_IPC4_GTW_SSP;
        sspCopierParams.NodeIndex = SOF_IPC4_NODE_INDEX_INTEL_SSP(SspPort);
        sspCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&sspCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, sspInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, sspInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;
    } else {
        /* Capture: SSP DAI Copier -> Gain -> Host Copier */
        sspCopierParams.StreamTag = StreamTag;
        sspCopierParams.NodeType = SOF_IPC4_GTW_SSP;
        sspCopierParams.NodeIndex = SOF_IPC4_NODE_INDEX_INTEL_SSP(SspPort);
        sspCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&sspCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, sspInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        hostCopierParams.StreamTag = StreamTag;
        hostCopierParams.NodeType = SOF_IPC4_GTW_HOST_IN;
        hostCopierParams.NodeIndex = StreamTag - 1;
        hostCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, sspInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, hostInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;
    }

    status = SofIpc4SetPipelineState(Ctx, PipelineId, SOF_IPC4_PIPE_PAUSED);
    if (NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[SSP%u] Pipeline %u (%s) created and set to PAUSED (Host=%u, Gain=%u, SSP=%u)\n",
            SspPort, PipelineId, IsCapture ? "Capture" : "Playback", hostInst, gainInst, sspInst);
        SofLogBreadcrumb(bmsg);
    }

    if (OutHostInst) *OutHostInst = hostInst;
    if (OutGainInst) *OutGainInst = gainInst;
    if (OutDaiInst) *OutDaiInst = sspInst;
    return status;

cleanup:
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, sspInst);
    SofIpc4DeletePipeline(Ctx, PipelineId);
    return status;
}

NTSTATUS SofIpc4CreateDmicPipeline(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG PipelineId,
    _In_ ULONG StreamTag,
    _In_ const PSOF_AUDIO_FORMAT_PARAMS Fmt,
    _Out_opt_ PULONG OutHostInst,
    _Out_opt_ PULONG OutGainInst,
    _Out_opt_ PULONG OutDaiInst
)
{
    NTSTATUS status;
    UCHAR copierBuffer[256];
    ULONG copierLen;
    UCHAR gainBuffer[128];
    ULONG gainLen;
    SOF_COPIER_PARAMS dmicCopierParams = {0};
    SOF_COPIER_PARAMS hostCopierParams = {0};
    ULONG hostInst = 0, gainInst = 0, dmicInst = 0;

    if (OutHostInst) *OutHostInst = 0xFFFFFFFF;
    if (OutGainInst) *OutGainInst = 0xFFFFFFFF;
    if (OutDaiInst) *OutDaiInst = 0xFFFFFFFF;

    /* 1. Create pipeline with priority 0, 16 memory pages, Core 0 */
    status = SofIpc4CreatePipeline(Ctx, PipelineId, 0, 16, 0, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[DMIC] CreatePipeline(ID %u) failed: 0x%08x\n", PipelineId, status);
        SofLogBreadcrumb(bmsg);
        return status;
    }

    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &dmicInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, &gainInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &hostInst);

    /* Capture: DMIC DAI Copier -> Gain -> Host In Copier */
    dmicCopierParams.StreamTag = StreamTag;
    dmicCopierParams.NodeType = SOF_IPC4_GTW_DMIC;
    dmicCopierParams.NodeIndex = SOF_IPC4_NODE_INDEX_INTEL_DMIC(0);
    dmicCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&dmicCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, dmicInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    hostCopierParams.StreamTag = StreamTag;
    hostCopierParams.NodeType = SOF_IPC4_GTW_HOST_IN;
    hostCopierParams.NodeIndex = StreamTag - 1;
    hostCopierParams.AudioFormat = *Fmt;
    status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, dmicInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, hostInst, 0);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = SofIpc4SetPipelineState(Ctx, PipelineId, SOF_IPC4_PIPE_PAUSED);
    if (NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[DMIC] Pipeline %u (Capture) created and set to PAUSED (Host=%u, Gain=%u, DMIC=%u)\n",
            PipelineId, hostInst, gainInst, dmicInst);
        SofLogBreadcrumb(bmsg);
    }

    if (OutHostInst) *OutHostInst = hostInst;
    if (OutGainInst) *OutGainInst = gainInst;
    if (OutDaiInst) *OutDaiInst = dmicInst;
    return status;

cleanup:
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, dmicInst);
    SofIpc4DeletePipeline(Ctx, PipelineId);
    return status;
}

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
)
{
    NTSTATUS status;
    UCHAR copierBuffer[384];
    ULONG copierLen;
    UCHAR gainBuffer[128];
    ULONG gainLen;
    SOF_COPIER_PARAMS alhCopierParams = {0};
    SOF_COPIER_PARAMS hostCopierParams = {0};
    ULONG hostInst = 0, gainInst = 0, alhInst = 0;
    ULONG alhStreamId;

    if (OutHostInst) *OutHostInst = 0xFFFFFFFF;
    if (OutGainInst) *OutGainInst = 0xFFFFFFFF;
    if (OutDaiInst) *OutDaiInst = 0xFFFFFFFF;

    /* 1. Calculate ALH Stream ID using validated Linux kernel formula:
     * alh_stream_id = (LinkIndex * 16) + pdi_num + 3 (+ 2 if pdi_num >= 2)
     * For primary playback/render, PDI 0 is used.
     * For primary capture, PDI 1 is used.
     */
    {
        ULONG pdiNum = IsCapture ? 1 : 0;
        alhStreamId = (LinkIndex * 16) + pdiNum + 3;
        if (pdiNum >= 2) {
            alhStreamId += 2;
        }
    }

    /* 2. Create pipeline with priority 0, 16 memory pages, Core 0 */
    status = SofIpc4CreatePipeline(Ctx, PipelineId, 0, 16, 0, 0);
    if (!NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[ALH] CreatePipeline(ID %u) failed: 0x%08x\n", PipelineId, status);
        SofLogBreadcrumb(bmsg);
        return status;
    }

    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &hostInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, &gainInst);
    SofIpc4AllocInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, &alhInst);

    if (!IsCapture) {
        /* Render / Playback: Host Copier -> Gain -> ALH DAI Copier */
        hostCopierParams.StreamTag = StreamTag;
        hostCopierParams.NodeType = SOF_IPC4_GTW_HOST;
        hostCopierParams.NodeIndex = StreamTag - 1;
        hostCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        alhCopierParams.StreamTag = StreamTag;
        alhCopierParams.NodeType = SOF_IPC4_GTW_ALH;
        alhCopierParams.NodeIndex = 0x50 + (LinkIndex & 0x07);
        alhCopierParams.AlhStreamId = alhStreamId;
        alhCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&alhCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, alhInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, alhInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;

    } else {
        /* Capture: ALH DAI Copier -> Gain -> Host In Copier */
        alhCopierParams.StreamTag = StreamTag;
        alhCopierParams.NodeType = SOF_IPC4_GTW_ALH;
        alhCopierParams.NodeIndex = 0x50 + (LinkIndex & 0x07);
        alhCopierParams.AlhStreamId = alhStreamId;
        alhCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&alhCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, alhInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BuildGainConfig(Fmt->ChannelsCount, SOF_IPC4_VOLUME_ZERO_DB, gainBuffer, sizeof(gainBuffer), &gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, PipelineId, 0, gainBuffer, gainLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        hostCopierParams.StreamTag = StreamTag;
        hostCopierParams.NodeType = SOF_IPC4_GTW_HOST_IN;
        hostCopierParams.NodeIndex = StreamTag - 1;
        hostCopierParams.AudioFormat = *Fmt;
        status = SofIpc4BuildCopierConfig(&hostCopierParams, copierBuffer, sizeof(copierBuffer), &copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4InitModuleInstance(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst, PipelineId, 0, copierBuffer, copierLen);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_COPIER, alhInst, 0, SOF_IPC4_MOD_ID_GAIN, gainInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = SofIpc4BindModules(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst, 0, SOF_IPC4_MOD_ID_COPIER, hostInst, 0);
        if (!NT_SUCCESS(status)) goto cleanup;
    }

    status = SofIpc4SetPipelineState(Ctx, PipelineId, SOF_IPC4_PIPE_PAUSED);
    if (NT_SUCCESS(status)) {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[ALH] Pipeline %u (%s, Link %u, StreamID %u) created (Host=%u, Gain=%u, ALH=%u)\n",
            PipelineId, IsCapture ? "Capture" : "Render", LinkIndex, alhStreamId, hostInst, gainInst, alhInst);
        SofLogBreadcrumb(bmsg);
    }

    if (OutHostInst) *OutHostInst = hostInst;
    if (OutGainInst) *OutGainInst = gainInst;
    if (OutDaiInst) *OutDaiInst = alhInst;
    return status;

cleanup:
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(Ctx, SOF_IPC4_MOD_ID_COPIER, alhInst);
    SofIpc4DeletePipeline(Ctx, PipelineId);
    return status;
}
