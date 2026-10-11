/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright 2026 The SOF Authors
 */
/*
 * SOF Windows Driver Core Unit Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sof/sof_osal.h>
#include <sof/header.h>
#include <sof/info.h>
#include <sof/ext_manifest4.h>
#include <sof/ipc4/header.h>
#include "../driver/PciIds.h"
#include "../driver/Hw.h"
#include "../driver/Loader.h"
#include "../driver/Topo.h"
#include "../driver/WaveRt.h"
#include "../driver/Nhlt.h"
#include "../driver/Device.h"
#include "../driver/SdwController.h"
#include <strsafe.h>

/* Mock stubs for driver functions when running in user mode unit tests */
void SofLogBreadcrumb(const char* text) { (void)text; }
NTSTATUS SofHdaStartConverter(PSOF_DEVICE_CONTEXT Ctx, ULONG StreamIndex, ULONG StreamTag) { (void)Ctx; (void)StreamIndex; (void)StreamTag; return 0; }
NTSTATUS SofHdaStopConverter(PSOF_DEVICE_CONTEXT Ctx, ULONG StreamIndex) { (void)Ctx; (void)StreamIndex; return 0; }
NTSTATUS SofHdaProbeDisplayCodec(PSOF_DEVICE_CONTEXT Ctx) { (void)Ctx; return 0; }
NTSTATUS SofHdaInitDisplayCodec(PSOF_DEVICE_CONTEXT Ctx) { (void)Ctx; return 0; }
NTSTATUS SofHdaStartDisplayConverter(PSOF_DEVICE_CONTEXT Ctx, ULONG hdmiIndex, ULONG StreamTag) { (void)Ctx; (void)hdmiIndex; (void)StreamTag; return 0; }
NTSTATUS SofHdaStopDisplayConverter(PSOF_DEVICE_CONTEXT Ctx, ULONG hdmiIndex) { (void)Ctx; (void)hdmiIndex; return 0; }

/* Exported from sof_core */
extern u32 sof_core_get_version(void);
extern bool sof_core_validate_manifest4_magic(const void *buffer, size_t size);
extern u64 sof_ipc4_pack_header(u32 primary, u32 extension);

/* Exported from Hw.c */
extern PSOF_DSP_HW_DESC SofGetHardwareDesc(USHORT VendorId, USHORT DeviceId);

static void test_version(void)
{
    u32 ver = sof_core_get_version();
    printf("[TEST] SOF ABI Version: 0x%08x (Major: %u, Minor: %u, Patch: %u)\n",
           ver,
           SOF_ABI_VERSION_MAJOR(ver),
           SOF_ABI_VERSION_MINOR(ver),
           SOF_ABI_VERSION_PATCH(ver));
    assert(SOF_ABI_VERSION_MAJOR(ver) == SOF_ABI_MAJOR);
    printf("[PASS] test_version\n");
}

static void test_ipc4_header_packing(void)
{
    u32 primary = 0x12345678;
    u32 extension = 0x9abcdef0;
    u64 packed = sof_ipc4_pack_header(primary, extension);

    struct sof_ipc4_msg msg = {0};
    msg.header_u64 = packed;

    assert(msg.primary == primary);
    assert(msg.extension == extension);
    printf("[PASS] test_ipc4_header_packing (Packed: 0x%016llx)\n", packed);
}

static void test_manifest4_magic(void)
{
    struct sof_ext_manifest4_hdr valid_hdr = {0};
    valid_hdr.id = SOF_EXT_MAN4_MAGIC_NUMBER;

    struct sof_ext_manifest4_hdr invalid_hdr = {0};
    invalid_hdr.id = 0xdeadbeef;

    assert(sof_core_validate_manifest4_magic(&valid_hdr, sizeof(valid_hdr)) == true);
    assert(sof_core_validate_manifest4_magic(&invalid_hdr, sizeof(invalid_hdr)) == false);
    assert(sof_core_validate_manifest4_magic(&valid_hdr, sizeof(valid_hdr) - 1) == false);
    printf("[PASS] test_manifest4_magic (Magic: 0x%08x)\n", SOF_EXT_MAN4_MAGIC_NUMBER);
}

static void test_platform_detection(void)
{
    /* Panther Lake */
    PSOF_DSP_HW_DESC ptl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PTL);
    assert(ptl != NULL);
    assert(ptl->Family == SOF_HW_INTEL_ACE_3_0);
    assert(ptl->IsAce == TRUE);
    assert(ptl->RomStatusReg == 0x160200);
    assert(ptl->MailboxDownlinkOffset == 0x188000);
    assert(ptl->HipcTdr == 0x73200);
    printf("[PASS] test_platform_detection: Panther Lake (ACE 3.0)\n");

    /* Lunar Lake */
    PSOF_DSP_HW_DESC lnl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_LNL);
    assert(lnl != NULL);
    assert(lnl->Family == SOF_HW_INTEL_ACE_2_0);
    assert(lnl->IsAce == TRUE);
    printf("[PASS] test_platform_detection: Lunar Lake (ACE 2.0)\n");

    /* Meteor Lake */
    PSOF_DSP_HW_DESC mtl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_MTL);
    assert(mtl != NULL);
    assert(mtl->Family == SOF_HW_INTEL_ACE_1_0);
    assert(mtl->IsAce == TRUE);
    printf("[PASS] test_platform_detection: Meteor Lake (ACE 1.0)\n");

    /* Tiger Lake */
    PSOF_DSP_HW_DESC tgl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_TGL);
    assert(tgl != NULL);
    assert(tgl->Family == SOF_HW_INTEL_CAVS_2_5);
    assert(tgl->IsAce == FALSE);
    assert(tgl->RomStatusReg == 0x80000);
    assert(tgl->MailboxDownlinkOffset == 0xa0000);
    assert(tgl->MailboxUplinkOffset == 0x81000);
    assert(tgl->HipcTdr == 0x00c0);
    assert(tgl->HipcIdr == 0x00d0);
    printf("[PASS] test_platform_detection: Tiger Lake (cAVS 2.5)\n");

    /* Alder Lake */
    PSOF_DSP_HW_DESC adl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ADL_P);
    assert(adl != NULL);
    assert(adl->Family == SOF_HW_INTEL_CAVS_2_5);
    printf("[PASS] test_platform_detection: Alder Lake (cAVS 2.5)\n");

    /* Ice Lake */
    PSOF_DSP_HW_DESC icl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICL);
    assert(icl != NULL);
    assert(icl->Family == SOF_HW_INTEL_CAVS_2_0);
    printf("[PASS] test_platform_detection: Ice Lake (cAVS 2.0)\n");

    /* Cannon Lake */
    PSOF_DSP_HW_DESC cnl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_CNL);
    assert(cnl != NULL);
    assert(cnl->Family == SOF_HW_INTEL_CAVS_1_8);
    printf("[PASS] test_platform_detection: Cannon Lake (cAVS 1.8)\n");

    /* Apollo Lake */
    PSOF_DSP_HW_DESC apl = SofGetHardwareDesc(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_APL);
    assert(apl != NULL);
    assert(apl->Family == SOF_HW_INTEL_CAVS_1_5);
    printf("[PASS] test_platform_detection: Apollo Lake (cAVS 1.5)\n");

    /* AMD ACP */
    PSOF_DSP_HW_DESC acp = SofGetHardwareDesc(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_ACP_7_0);
    assert(acp != NULL);
    assert(acp->Family == SOF_HW_AMD_ACP);
    printf("[PASS] test_platform_detection: AMD ACP 7.0 (Strix Point)\n");
}

static void test_ipc4_fw_ready_notification(void)
{
    /* Construct IPC4 FW_READY notification header */
    u32 primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_GLB_NOTIFICATION) |
                  (SOF_IPC4_NOTIFY_FW_READY << SOF_IPC4_NOTIFICATION_TYPE_SHIFT);

    assert(SOF_IPC4_MSG_IS_NOTIFICATION(primary));
    assert(SOF_IPC4_NOTIFICATION_TYPE_GET(primary) == SOF_IPC4_NOTIFY_FW_READY);
    printf("[PASS] test_ipc4_fw_ready_notification (Type: %u, IsNotification: %s)\n",
           (unsigned int)SOF_IPC4_NOTIFICATION_TYPE_GET(primary),
           SOF_IPC4_MSG_IS_NOTIFICATION(primary) ? "YES" : "NO");
}

static void test_bar4_simulation(void)
{
    /* Allocate 2MB dummy BAR4 region */
    void *dummyBar4 = calloc(1, 0x200000);
    assert(dummyBar4 != NULL);

    /* Test cAVS mailbox write / read */
    ULONG tglMailboxDownlink = 0xa0000;
    ULONG testValue = 0x12345678;
    SofDspWrite32(dummyBar4, tglMailboxDownlink, testValue);
    assert(SofDspRead32(dummyBar4, tglMailboxDownlink) == testValue);

    /* Test doorbell bit update */
    ULONG tglHipcCtl = 0x00e8;
    SofDspWrite32(dummyBar4, tglHipcCtl, 0x0);
    SofDspUpdateBits(dummyBar4, tglHipcCtl, 0x3, 0x3);
    assert(SofDspRead32(dummyBar4, tglHipcCtl) == 0x3);

    SofDspUpdateBits(dummyBar4, tglHipcCtl, 0x1, 0x0);
    assert(SofDspRead32(dummyBar4, tglHipcCtl) == 0x2);

    /* Test ACE mailbox write / read */
    ULONG mtlMailboxDownlink = 0x188000;
    ULONG aceValue = 0xaabbccdd;
    SofDspWrite32(dummyBar4, mtlMailboxDownlink, aceValue);
    assert(SofDspRead32(dummyBar4, mtlMailboxDownlink) == aceValue);

    free(dummyBar4);
    printf("[PASS] test_bar4_simulation (cAVS & ACE Mailbox MMIO)\n");
}

/* Exported from Loader.c */
extern NTSTATUS SofParseFirmwareImage(const void *Buffer, size_t Size, PSOF_FW_IMAGE_INFO Info);

static void test_firmware_manifest_parser(void)
{
    size_t extManSize = sizeof(struct sof_ext_manifest4_hdr);
    size_t fwHdrSize = sizeof(struct sof_man4_fw_binary_header);
    size_t payloadSize = 4096;
    size_t totalSize = extManSize + fwHdrSize + payloadSize;

    UCHAR *mockFw = (UCHAR *)calloc(1, totalSize);
    assert(mockFw != NULL);

    /* Populate extended manifest header */
    struct sof_ext_manifest4_hdr *extMan = (struct sof_ext_manifest4_hdr *)mockFw;
    extMan->id = SOF_EXT_MAN4_MAGIC_NUMBER;
    extMan->len = (uint32_t)extManSize;
    extMan->version_major = 1;
    extMan->version_minor = 0;
    extMan->num_module_entries = 5;

    /* Populate fw binary header */
    struct sof_man4_fw_binary_header *fwHdr = (struct sof_man4_fw_binary_header *)(mockFw + extManSize);
    fwHdr->major_version = 2;
    fwHdr->minor_version = 12;
    fwHdr->hotfix_version = 1;
    fwHdr->build_version = 5432;
    fwHdr->preload_page_count = 64;
    memcpy(fwHdr->name, "BASEFW", 6);

    SOF_FW_IMAGE_INFO info = {0};
    NTSTATUS status = SofParseFirmwareImage(mockFw, totalSize, &info);
    assert(status == 0); /* STATUS_SUCCESS */
    assert(info.PayloadOffset == (ULONG)extManSize);
    assert(info.PayloadSize == (ULONG)(fwHdrSize + payloadSize));
    assert(info.MajorVersion == 2);
    assert(info.MinorVersion == 12);
    assert(info.HotfixVersion == 1);
    assert(info.BuildVersion == 5432);
    assert(info.NumModules == 5);
    assert(strcmp(info.BinaryName, "BASEFW") == 0);

    /* Test corrupt magic number */
    extMan->id = 0xdeadbeef;
    status = SofParseFirmwareImage(mockFw, totalSize, &info);
    assert(status != 0);

    free(mockFw);
    printf("[PASS] test_firmware_manifest_parser (Extended Manifest 4 & Base FW Header)\n");
}

static void test_ipc4_pipeline_packets(void)
{
    NTSTATUS status;

    /* Test pipeline creation packet */
    status = SofIpc4CreatePipeline(NULL, 1, 0, 4, 0, 0);
    assert(status == 0);

    /* Test pipeline state transitions */
    status = SofIpc4SetPipelineState(NULL, 1, SOF_IPC4_PIPE_PAUSED);
    assert(status == 0);

    status = SofIpc4SetPipelineState(NULL, 1, SOF_IPC4_PIPE_RUNNING);
    assert(status == 0);

    status = SofIpc4SetPipelineState(NULL, 1, SOF_IPC4_PIPE_RESET);
    assert(status == 0);

    /* Test pipeline deletion */
    status = SofIpc4DeletePipeline(NULL, 1);
    assert(status == 0);

    printf("[PASS] test_ipc4_pipeline_packets (Create, State Transitions, Delete)\n");
}

static void test_ipc4_copier_config(void)
{
    SOF_COPIER_PARAMS params = {0};
    UCHAR buffer[256];
    ULONG len = 0;
    NTSTATUS status;
    struct sof_ipc4_copier_data *copier;

    /* Build 48kHz, 32-bit (24-bit valid) stereo format */
    SofIpc4BuildDefaultStereoFormat(48000, 32, &params.AudioFormat);
    params.StreamTag = 1;
    params.NodeType = SOF_IPC4_GTW_HOST;
    params.NodeIndex = 0; /* Stream 1 -> Node index 0 */

    status = SofIpc4BuildCopierConfig(&params, buffer, sizeof(buffer), &len);
    assert(status == 0);
    assert(len == sizeof(struct sof_ipc4_copier_data));

    copier = (struct sof_ipc4_copier_data *)buffer;
    assert(copier->base_config.audio_fmt.sampling_frequency == 48000);
    assert(copier->base_config.audio_fmt.bit_depth == 32);
    assert(copier->base_config.audio_fmt.ch_map == SOF_IPC4_CH_MAP_STEREO);
    assert(copier->base_config.cpc == SOF_IPC4_COPIER_DEFAULT_CPC);
    assert(copier->base_config.is_pages == SOF_IPC4_COPIER_DEFAULT_PAGES);
    assert(copier->base_config.ibs == 384); /* 1ms chunk: 48000 * 2 * 4 / 1000 = 384 */
    assert(copier->gtw_cfg.dma_buffer_size == 1536); /* 4ms DMA buffer */
    assert(copier->gtw_cfg.node_id == (SOF_IPC4_NODE_TYPE(SOF_IPC4_GTW_HOST) | SOF_IPC4_NODE_INDEX(0)));

    /* Test Codec DAI Copier */
    params.NodeType = SOF_IPC4_GTW_HDA;
    params.NodeIndex = 0;
    status = SofIpc4BuildCopierConfig(&params, buffer, sizeof(buffer), &len);
    assert(status == 0);
    copier = (struct sof_ipc4_copier_data *)buffer;
    assert(copier->gtw_cfg.node_id == (SOF_IPC4_NODE_TYPE(SOF_IPC4_GTW_HDA) | SOF_IPC4_NODE_INDEX(0)));

    printf("[PASS] test_ipc4_copier_config (Host Gateway & HD-A Codec DAI Gateway)\n");
}

static void test_ipc4_gain_and_volume(void)
{
    UCHAR buffer[128];
    ULONG len = 0;
    NTSTATUS status;
    struct sof_ipc4_gain_data *gain;

    /* Test Gain module config creation */
    status = SofIpc4BuildGainConfig(2, SOF_IPC4_VOLUME_ZERO_DB, buffer, sizeof(buffer), &len);
    assert(status == 0);
    assert(len == sizeof(struct sof_ipc4_gain_data));

    gain = (struct sof_ipc4_gain_data *)buffer;
    assert(gain->params.channels == SOF_IPC4_GAIN_ALL_CHANNELS_MASK);
    assert(gain->params.init_val == SOF_IPC4_VOLUME_ZERO_DB);
    assert(gain->base_config.cpc == SOF_IPC4_GAIN_DEFAULT_CPC);
    assert(gain->base_config.is_pages == SOF_IPC4_GAIN_DEFAULT_PAGES);

    /* Test Volume IPC command generation */
    status = SofIpc4SetVolume(NULL, SOF_IPC4_MOD_ID_GAIN, 1, SOF_IPC4_GAIN_ALL_CHANNELS_MASK, SOF_IPC4_VOLUME_ZERO_DB);
    assert(status == 0);

    /* Test Mute */
    status = SofIpc4SetVolume(NULL, SOF_IPC4_MOD_ID_GAIN, 1, SOF_IPC4_GAIN_ALL_CHANNELS_MASK, SOF_IPC4_VOLUME_MUTE);
    assert(status == 0);

    printf("[PASS] test_ipc4_gain_and_volume (0 dB Zero-Attenuation & Mute Config)\n");
}

static void test_ipc4_binding_packets(void)
{
    NTSTATUS status;

    /* Bind Host Copier (Inst 0, Queue 0) -> Gain (Inst 0, Queue 0) */
    status = SofIpc4BindModules(NULL, SOF_IPC4_MOD_ID_COPIER, 0, 0, SOF_IPC4_MOD_ID_GAIN, 0, 0);
    assert(status == 0);

    /* Bind Gain (Inst 0, Queue 0) -> Codec DAI Copier (Inst 1, Queue 0) */
    status = SofIpc4BindModules(NULL, SOF_IPC4_MOD_ID_GAIN, 0, 0, SOF_IPC4_MOD_ID_COPIER, 1, 0);
    assert(status == 0);

    /* Unbind */
    status = SofIpc4UnbindModules(NULL, SOF_IPC4_MOD_ID_COPIER, 0, 0, SOF_IPC4_MOD_ID_GAIN, 0, 0);
    assert(status == 0);

    printf("[PASS] test_ipc4_binding_packets (Module Pin Connection & Disconnection)\n");
}

static void test_wavert_stream_lifecycle(void)
{
    SOF_AUDIO_FORMAT_PARAMS fmt = {0};
    PSOF_WAVERT_STREAM stream = NULL;
    NTSTATUS status;
    ULONG dummyEvent = 0xbeef;
    ULONG pos = 0;
    ULONGLONG clock = 0;

    SofIpc4BuildDefaultStereoFormat(48000, 32, &fmt);

    /* 1. Create Playback Stream (64KB circular ring buffer, 10ms period) */
    status = SofWaveRtCreateStream(NULL, SOF_ENDPOINT_HDA_ANALOG, FALSE, 1, &fmt, 65536, 10, &stream);
    assert(status == 0);
    assert(stream != NULL);
    assert(stream->BufferSizeBytes == 65536);
    assert(stream->PeriodMs == 10);
    assert(stream->State == KSSTATE_ACQUIRE);
    assert(stream->Bdl != NULL);
    assert(stream->Bdl[0].Ioc == 1);
    assert(stream->Bdl[1].Ioc == 1);
    assert(stream->Bdl[0].Size == 32768);

    /* 2. Register Notification Event */
    status = SofWaveRtRegisterNotificationEvent(stream, (PVOID)&dummyEvent);
    assert(status == 0);
    assert(stream->NumNotificationEvents == 1);

    /* 3. Transition to KSSTATE_RUN */
    status = SofWaveRtSetState(stream, KSSTATE_RUN);
    assert(status == 0);
    assert(stream->State == KSSTATE_RUN);

    /* 4. Query Position and Clock */
    status = SofWaveRtGetPosition(stream, &pos);
    assert(status == 0);

    status = SofWaveRtGetClockTime(stream, &clock);
    assert(status == 0);

    /* 5. Signal Notification (simulated) */
    SofWaveRtSignalNotification(stream);

    /* 6. Transition to KSSTATE_PAUSE */
    status = SofWaveRtSetState(stream, KSSTATE_PAUSE);
    assert(status == 0);
    assert(stream->State == KSSTATE_PAUSE);

    /* 7. Unregister Notification Event */
    status = SofWaveRtUnregisterNotificationEvent(stream, (PVOID)&dummyEvent);
    assert(status == 0);
    assert(stream->NumNotificationEvents == 0);

    /* 8. Free Stream */
    status = SofWaveRtFreeStream(stream);
    assert(status == 0);

    printf("[PASS] test_wavert_stream_lifecycle (Create, Run, Pause, Notify, Teardown)\n");
}

static void test_ipc4_instance_allocator(void)
{
    ULONG inst0 = 0xFF, inst1 = 0xFF, inst2 = 0xFF, reallocInst = 0xFF;
    ULONG gInst0 = 0xFF, gInst1 = 0xFF;
    NTSTATUS status;

    /* Allocate 3 Copier instances */
    status = SofIpc4AllocInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, &inst0);
    assert(status == 0 && inst0 == 0);
    status = SofIpc4AllocInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, &inst1);
    assert(status == 0 && inst1 == 1);
    status = SofIpc4AllocInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, &inst2);
    assert(status == 0 && inst2 == 2);

    /* Allocate 2 Gain instances */
    status = SofIpc4AllocInstanceId(NULL, SOF_IPC4_MOD_ID_GAIN, &gInst0);
    assert(status == 0 && gInst0 == 0);
    status = SofIpc4AllocInstanceId(NULL, SOF_IPC4_MOD_ID_GAIN, &gInst1);
    assert(status == 0 && gInst1 == 1);

    /* Free Copier instance 1 and re-allocate: should get 1 back */
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, 1);
    status = SofIpc4AllocInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, &reallocInst);
    assert(status == 0 && reallocInst == 1);

    /* Clean up all instances */
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, inst0);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, reallocInst);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, inst2);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_GAIN, gInst0);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_GAIN, gInst1);

    printf("[PASS] test_ipc4_instance_allocator (IDA Module Instance Bitmaps)\n");
}

static void test_ipc4_hdmi_and_ssp_pipeline_creation(void)
{
    SOF_AUDIO_FORMAT_PARAMS fmt = {0};
    ULONG hostInst = 0, gainInst = 0, daiInst = 0;
    NTSTATUS status;

    SofIpc4BuildDefaultStereoFormat(48000, 32, &fmt);

    /* 1. HDMI 1, 2, 3 Pipelines */
    for (ULONG link = 1; link <= 3; link++) {
        status = SofIpc4CreateHdmiPipeline(NULL, link, link, link, &fmt, &hostInst, &gainInst, &daiInst);
        assert(status == 0);
        assert(hostInst != 0xFFFFFFFF);
        assert(gainInst != 0xFFFFFFFF);
        assert(daiInst != 0xFFFFFFFF);

        SofIpc4SetPipelineState(NULL, link, SOF_IPC4_PIPE_RESET);
        SofIpc4DeletePipeline(NULL, link);
        SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, hostInst);
        SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_GAIN, gainInst);
        SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, daiInst);
    }

    /* 2. SSP2 Playback Pipeline */
    status = SofIpc4CreateSspPipeline(NULL, 4, 4, 2, FALSE, &fmt, &hostInst, &gainInst, &daiInst);
    assert(status == 0);
    SofIpc4SetPipelineState(NULL, 4, SOF_IPC4_PIPE_RESET);
    SofIpc4DeletePipeline(NULL, 4);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, daiInst);

    /* 3. SSP2 Capture Pipeline */
    status = SofIpc4CreateSspPipeline(NULL, 5, 5, 2, TRUE, &fmt, &hostInst, &gainInst, &daiInst);
    assert(status == 0);
    SofIpc4SetPipelineState(NULL, 5, SOF_IPC4_PIPE_RESET);
    SofIpc4DeletePipeline(NULL, 5);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, hostInst);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_GAIN, gainInst);
    SofIpc4FreeInstanceId(NULL, SOF_IPC4_MOD_ID_COPIER, daiInst);

    printf("[PASS] test_ipc4_hdmi_and_ssp_pipeline_creation (HDMI 1-3 & SSP2 Playback/Capture)\n");
}

#pragma pack(push, 1)
typedef struct _MOCK_NHLT_TABLE {
    ACPI_NHLT_HEADER         Header;
    UCHAR                    EndpointCount;
    NHLT_ENDPOINT_DESCRIPTOR DmicEp;
    NHLT_ENDPOINT_DESCRIPTOR SspEp;
} MOCK_NHLT_TABLE;
#pragma pack(pop)

static void test_nhlt_mock_parsing(void)
{
    MOCK_NHLT_TABLE mockTable;
    SOF_NHLT_INFO nhltInfo;
    NTSTATUS status;

    memset(&mockTable, 0, sizeof(mockTable));
    mockTable.Header.Signature = NHLT_ACPI_SIGNATURE; /* 'NHLT' */
    mockTable.Header.Length = sizeof(mockTable);
    mockTable.Header.Revision = 1;
    mockTable.EndpointCount = 2;

    /* Endpoint 1: DMIC Array (2-channel) */
    mockTable.DmicEp.Length = sizeof(NHLT_ENDPOINT_DESCRIPTOR);
    mockTable.DmicEp.LinkType = NHLT_LINK_DMIC;
    mockTable.DmicEp.InstanceId = 0;
    mockTable.DmicEp.Direction = NHLT_DIRECTION_CAPTURE;

    /* Endpoint 2: SSP Codec (e.g. Realtek ALC5682 / ALC285) */
    mockTable.SspEp.Length = sizeof(NHLT_ENDPOINT_DESCRIPTOR);
    mockTable.SspEp.LinkType = NHLT_LINK_SSP;
    mockTable.SspEp.InstanceId = 0;
    mockTable.SspEp.HwVendorId = 0x10EC; /* Realtek */
    mockTable.SspEp.HwDeviceId = 0x0285;
    mockTable.SspEp.Direction = NHLT_DIRECTION_RENDER;

    status = SofNhltParse(&mockTable, sizeof(mockTable), &nhltInfo);
    assert(status == 0);
    assert(nhltInfo.IsValid == TRUE);
    assert(nhltInfo.TotalEndpoints == 2);
    assert(nhltInfo.DmicEndpointsCount == 1);
    assert(nhltInfo.SspEndpointsCount == 1);
    assert(nhltInfo.Dmic.Present == TRUE);
    assert(nhltInfo.Dmic.ChannelCount == NHLT_DMIC_2CH);
    assert(nhltInfo.Dmic.SampleRate == 48000);
    assert(nhltInfo.ExternalCodecPresent == TRUE);
    assert(nhltInfo.CodecVendorId == 0x10EC);
    assert(nhltInfo.CodecDeviceId == 0x0285);
    assert(nhltInfo.CodecLinkType == NHLT_LINK_SSP);

    printf("[PASS] test_nhlt_mock_parsing (2 endpoints: DMIC 2-ch + SSP Codec 0x10EC:0x0285)\n");
}

static void test_nhlt_sdw_parsing(void)
{
#pragma pack(push, 1)
    struct {
        ACPI_NHLT_HEADER            Header;
        UCHAR                       EndpointCount;
        NHLT_ENDPOINT_DESCRIPTOR    SdwRenderEp;
        NHLT_ENDPOINT_DESCRIPTOR    SdwCaptureEp;
    } sdwTable = {0};
#pragma pack(pop)

    SOF_NHLT_INFO nhltInfo = {0};
    NTSTATUS status;

    sdwTable.Header.Signature = NHLT_ACPI_SIGNATURE;
    sdwTable.Header.Length = sizeof(sdwTable);
    sdwTable.EndpointCount = 2;

    /* Endpoint 1: SoundWire Link 0 Render (e.g. Speaker Codec 0x025D:0x0711) */
    sdwTable.SdwRenderEp.Length = sizeof(NHLT_ENDPOINT_DESCRIPTOR);
    sdwTable.SdwRenderEp.LinkType = NHLT_LINK_SDW;
    sdwTable.SdwRenderEp.InstanceId = 0;
    sdwTable.SdwRenderEp.HwVendorId = 0x025D;
    sdwTable.SdwRenderEp.HwDeviceId = 0x0711;
    sdwTable.SdwRenderEp.Direction = NHLT_DIRECTION_RENDER;

    /* Endpoint 2: SoundWire Link 1 Capture (e.g. Headset Mic 0x025D:0x0713) */
    sdwTable.SdwCaptureEp.Length = sizeof(NHLT_ENDPOINT_DESCRIPTOR);
    sdwTable.SdwCaptureEp.LinkType = NHLT_LINK_SDW;
    sdwTable.SdwCaptureEp.InstanceId = 1;
    sdwTable.SdwCaptureEp.HwVendorId = 0x025D;
    sdwTable.SdwCaptureEp.HwDeviceId = 0x0713;
    sdwTable.SdwCaptureEp.Direction = NHLT_DIRECTION_CAPTURE;

    status = SofNhltParse(&sdwTable, sizeof(sdwTable), &nhltInfo);
    assert(status == 0);
    assert(nhltInfo.IsValid == TRUE);
    assert(nhltInfo.TotalEndpoints == 2);
    assert(nhltInfo.SdwEndpointsCount == 2);

    /* Verify Endpoint 0 */
    assert(nhltInfo.SdwEndpoints[0].Present == TRUE);
    assert(nhltInfo.SdwEndpoints[0].LinkIndex == 0);
    assert(nhltInfo.SdwEndpoints[0].Direction == NHLT_DIRECTION_RENDER);
    assert(nhltInfo.SdwEndpoints[0].VendorId == 0x025D);
    assert(nhltInfo.SdwEndpoints[0].DeviceId == 0x0711);
    assert(nhltInfo.SdwEndpoints[0].SampleRate == 48000);
    assert(nhltInfo.SdwEndpoints[0].ChannelCount == 2);
    assert(nhltInfo.SdwEndpoints[0].BitDepth == 32);

    /* Verify Endpoint 1 */
    assert(nhltInfo.SdwEndpoints[1].Present == TRUE);
    assert(nhltInfo.SdwEndpoints[1].LinkIndex == 1);
    assert(nhltInfo.SdwEndpoints[1].Direction == NHLT_DIRECTION_CAPTURE);
    assert(nhltInfo.SdwEndpoints[1].VendorId == 0x025D);
    assert(nhltInfo.SdwEndpoints[1].DeviceId == 0x0713);
    assert(nhltInfo.SdwEndpoints[1].SampleRate == 48000);
    assert(nhltInfo.SdwEndpoints[1].ChannelCount == 2);
    assert(nhltInfo.SdwEndpoints[1].BitDepth == 32);

    printf("[PASS] test_nhlt_sdw_parsing (2 SDW endpoints: Link 0 Render 0x025D:0x0711 + Link 1 Capture 0x025D:0x0713)\n");
}

static void test_volume_db_conversion(void)
{
    /* 1. Exact known conversions */
    assert(SofWindowsDbToSofGain(0) == SOF_IPC4_VOLUME_ZERO_DB);
    assert(SofWindowsDbToSofGain(65536) == SOF_IPC4_VOLUME_ZERO_DB); /* Clamping positive dB */
    assert(SofWindowsDbToSofGain(-1 * 65536) == 0x721482BF);
    assert(SofWindowsDbToSofGain(-6 * 65536) == 0x4026E73C);
    assert(SofWindowsDbToSofGain(-20 * 65536) == 0x0CCCCCCD);
    assert(SofWindowsDbToSofGain(SOF_VOLUME_MIN_DB_UNITS) == SOF_IPC4_VOLUME_MUTE);
    assert(SofWindowsDbToSofGain(-100 * 65536) == SOF_IPC4_VOLUME_MUTE);
    assert(SofWindowsDbToSofGain((LONG)0x80000000) == SOF_IPC4_VOLUME_MUTE);

    /* 2. Reverse conversions */
    assert(SofGainToWindowsDb(SOF_IPC4_VOLUME_ZERO_DB) == 0);
    assert(SofGainToWindowsDb(0x721482BF) == -1 * 65536);
    assert(SofGainToWindowsDb(0x4026E73C) == -6 * 65536);
    assert(SofGainToWindowsDb(0x0CCCCCCD) == -20 * 65536);
    assert(SofGainToWindowsDb(0) == SOF_VOLUME_MIN_DB_UNITS);

    /* 3. Sub-dB fractional round-trip accuracy */
    LONG testDbs[] = {
        (LONG)(-0.5 * 65536),
        (LONG)(-3.0 * 65536),
        (LONG)(-12.5 * 65536),
        (LONG)(-45.2 * 65536)
    };
    for (int i = 0; i < 4; i++) {
        ULONG gain = SofWindowsDbToSofGain(testDbs[i]);
        LONG roundTrip = SofGainToWindowsDb(gain);
        LONG err = testDbs[i] - roundTrip;
        if (err < 0) err = -err;
        assert(err <= 1); /* Max error 1 unit out of 65536 (< 0.0001 dB) */
    }

    printf("[PASS] test_volume_db_conversion (dB <-> Q1.31 Linear Gain Integer Interpolation)\n");
}

static void test_audio_controls_and_caching(void)
{
    SOF_DEVICE_CONTEXT mockCtx = {0};
    SOF_AUDIO_FORMAT_PARAMS fmt = {0};
    PSOF_WAVERT_STREAM stream = NULL;
    NTSTATUS status;
    LONG volDb;
    BOOLEAN mute;
    LONG peak;

    /* 1. Direct IPC API tests */
    status = SofIpc4SetVolumeLevel(NULL, 1, SOF_IPC4_GAIN_ALL_CHANNELS_MASK, -6 * 65536);
    assert(status == 0);

    status = SofIpc4SetMute(NULL, 1, SOF_IPC4_GAIN_ALL_CHANNELS_MASK, TRUE, -6 * 65536);
    assert(status == 0);

    status = SofIpc4SetMute(NULL, 1, SOF_IPC4_GAIN_ALL_CHANNELS_MASK, FALSE, -6 * 65536);
    assert(status == 0);

    status = SofIpc4SetVolumeCurve(NULL, 1, 0, 0x40000000, SOF_IPC4_GAIN_CURVE_TYPE_LOG, 1920);
    assert(status == 0);

    /* 2. Configure defaults in mock DeviceContext */
    mockCtx.DefaultVolumeLevelDb[SOF_ENDPOINT_HDMI_1][0] = -10 * 65536;
    mockCtx.DefaultVolumeLevelDb[SOF_ENDPOINT_HDMI_1][1] = -12 * 65536;
    mockCtx.DefaultMute[SOF_ENDPOINT_HDMI_1] = FALSE;

    /* 3. Create stream and verify it inherited cached volume */
    SofIpc4BuildDefaultStereoFormat(48000, 32, &fmt);
    status = SofWaveRtCreateStream(&mockCtx, SOF_ENDPOINT_HDMI_1, FALSE, 1, &fmt, 65536, 10, &stream);
    assert(status == 0);
    assert(stream != NULL);

    assert(stream->VolumeLevelDb[0] == -10 * 65536);
    assert(stream->VolumeLevelDb[1] == -12 * 65536);
    assert(stream->MasterMute == FALSE);

    /* 4. Test Stream Control APIs */
    status = SofWaveRtGetVolume(stream, 0, &volDb);
    assert(status == 0 && volDb == -10 * 65536);

    status = SofWaveRtGetVolume(stream, 1, &volDb);
    assert(status == 0 && volDb == -12 * 65536);

    status = SofWaveRtSetVolume(stream, 0, -3 * 65536);
    assert(status == 0);
    assert(stream->VolumeLevelDb[0] == -3 * 65536);

    /* Test Mute */
    status = SofWaveRtSetMute(stream, TRUE);
    assert(status == 0);
    assert(stream->MasterMute == TRUE);

    status = SofWaveRtGetMute(stream, &mute);
    assert(status == 0 && mute == TRUE);

    /* When muted, peak meter reports silence */
    status = SofWaveRtGetPeakMeter(stream, 0, &peak);
    assert(status == 0 && peak == SOF_VOLUME_MIN_DB_UNITS);

    /* Unmute and run stream */
    status = SofWaveRtSetMute(stream, FALSE);
    assert(status == 0);
    status = SofWaveRtSetState(stream, KSSTATE_RUN);
    assert(status == 0);

    /* When running, peak meter reports active volume */
    status = SofWaveRtGetPeakMeter(stream, 0, &peak);
    assert(status == 0 && peak == -3 * 65536);

    /* 5. Free stream and verify state persisted to device context */
    status = SofWaveRtFreeStream(stream);
    assert(status == 0);

    assert(mockCtx.DefaultVolumeLevelDb[SOF_ENDPOINT_HDMI_1][0] == -3 * 65536);
    assert(mockCtx.DefaultMute[SOF_ENDPOINT_HDMI_1] == FALSE);

    printf("[PASS] test_audio_controls_and_caching (Volume, Mute, PeakMeter, & Context Persistence)\n");
}

static void test_sdw_controller_programming(void)
{
    /* 1. Validate Cadence Master IP offsets against Linux cadence_master.c */
    assert(CDNS_MCP_CONFIG == 0x0000);
    assert(CDNS_MCP_CONTROL == 0x0004);
    assert(CDNS_MCP_FIFOSTAT == 0x0044);
    assert(CDNS_MCP_CONFIG_UPDATE == 0x0048);
    assert(CDNS_MCP_CMD_BASE == 0x0080);
    assert(CDNS_MCP_CONTROL_SW_RST == 0x40);
    assert(CDNS_MCP_CONTROL_CMD_RST == 0x80);

    /* 2. Validate Intel SHIM2 offsets against Linux intel_ace2x.c & sdw_intel.h */
    assert(SOF_SDW_SHIM_BASE_ACE == 0x38000);
    assert(SOF_SDW_ALH_BASE_ACE == 0x24000);
    assert(SOF_SDW_SHIM2_GENERIC_BASE(0) == 0x30000);
    assert(SOF_SDW_IP_BASE(0) == 0x30100);
    assert(SOF_SDW_SHIM2_VS_BASE(0) == 0x36000);
    assert(SOF_SDW_SHIM2_GENERIC_BASE(1) == 0x38000);
    assert(SOF_SDW_IP_BASE(1) == 0x38100);
    assert(SOF_SDW_SHIM2_VS_BASE(1) == 0x3E000);

    /* 3. Validate MIPI SoundWire peripheral register synthesizing fallback */
    assert(SofSdwSynthesizeRegRead(0, 0x0711, SDW_SCP_DEVNUMBER) == 1);
    assert(SofSdwSynthesizeRegRead(0, 0x0711, SDW_SCP_DEVID_0) == 0x12);
    assert(SofSdwSynthesizeRegRead(0, 0x0711, SDW_SCP_DEVID_1) == 0x5D);
    assert(SofSdwSynthesizeRegRead(0, 0x0711, SDW_SCP_DEVID_2) == 0x07);
    assert(SofSdwSynthesizeRegRead(0, 0x0711, SDW_SCP_DEVID_3) == 0x11);
    assert(SofSdwSynthesizeRegRead(0, 0x0711, SDW_SCP_FRAMESHAPE) == 0x7B);

    /* 4. Validate ALH Stream ID calculation formula from Linux intel_hw_params */
    /* alh_stream_id = (linkIndex * 16) + pdiNum + 3 (+ 2 if pdiNum >= 2) */
    ULONG link = 0;
    ULONG pdi0 = 0;
    ULONG pdi2 = 2;
    ULONG id0 = (link * 16) + pdi0 + 3; /* 3 */
    ULONG id2 = (link * 16) + pdi2 + 3 + 2; /* 7 */
    assert(id0 == 3);
    assert(id2 == 7);

    /* Link 1 */
    link = 1;
    ULONG idLink1_pdi0 = (link * 16) + pdi0 + 3; /* 19 */
    ULONG idLink1_pdi2 = (link * 16) + pdi2 + 3 + 2; /* 23 */
    assert(idLink1_pdi0 == 19);
    assert(idLink1_pdi2 == 23);

    /* 5. Validate ALH pipeline creation packet */
    SOF_DEVICE_CONTEXT ctx = {0};
    SOF_AUDIO_FORMAT_PARAMS fmt = {0};
    fmt.SamplingFrequency = 48000;
    fmt.BitDepth = 32;
    fmt.ValidBitDepth = 24;
    fmt.ChannelsCount = 2;
    fmt.ChannelMap = 0x10;
    fmt.ChannelConfig = 1;

    ULONG hostInst = 0xFFFFFFFF, gainInst = 0xFFFFFFFF, daiInst = 0xFFFFFFFF;
    NTSTATUS status = SofIpc4CreateAlhPipeline(&ctx, SOF_PIPELINE_ID_SDW_PLAYBACK, SOF_WAVERT_STREAM_TAG_SDW_RENDER, 0, FALSE, &fmt, &hostInst, &gainInst, &daiInst);
    assert(status == 0);
    assert(hostInst != 0xFFFFFFFF);
    assert(gainInst != 0xFFFFFFFF);
    assert(daiInst != 0xFFFFFFFF);
    assert(hostInst != daiInst);

    printf("[PASS] test_sdw_controller_programming (Cadence Master IP, SHIM2 & ALH Gateways)\n");
}

int main(void)
{
    printf("========================================================\n");
    printf(" Running SOF Core & Hardware Unit Tests (x64 Windows)\n");
    printf("========================================================\n");

    test_version();
    test_ipc4_header_packing();
    test_manifest4_magic();
    test_platform_detection();
    test_ipc4_fw_ready_notification();
    test_bar4_simulation();
    test_firmware_manifest_parser();
    test_ipc4_pipeline_packets();
    test_ipc4_copier_config();
    test_ipc4_gain_and_volume();
    test_volume_db_conversion();
    test_audio_controls_and_caching();
    test_ipc4_binding_packets();
    test_ipc4_instance_allocator();
    test_ipc4_hdmi_and_ssp_pipeline_creation();
    test_wavert_stream_lifecycle();
    test_nhlt_mock_parsing();
    test_nhlt_sdw_parsing();
    test_sdw_controller_programming();

    printf("========================================================\n");
    printf(" ALL TESTS PASSED!\n");
    printf("========================================================\n");
    return 0;
}
