/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SOF Core Library Entry and Version Verification
 */

#include <sof/sof_osal.h>
#include <sof/header.h>
#include <sof/info.h>
#include <sof/ext_manifest.h>
#include <sof/ext_manifest4.h>
#include <sof/control.h>
#include <sof/stream.h>
#include <sof/topology.h>
#include <sof/ipc4/header.h>

/**
 * sof_core_get_version - returns the compiled SOF ABI version number
 * Copyright 2026 The SOF Authors
 */
u32 sof_core_get_version(void)
{
    return SOF_ABI_VERSION;
}

/**
 * sof_core_validate_magic - validates that a given buffer starts with SOF extended manifest magic
 */
bool sof_core_validate_manifest4_magic(const void *buffer, size_t size)
{
    const struct sof_ext_manifest4_hdr *hdr;

    if (!buffer || size < sizeof(struct sof_ext_manifest4_hdr))
        return false;

    hdr = (const struct sof_ext_manifest4_hdr *)buffer;
    return hdr->id == SOF_EXT_MAN4_MAGIC_NUMBER;
}

/**
 * sof_ipc4_create_header - helper to pack IPC4 primary and extension 32-bit words
 */
u64 sof_ipc4_pack_header(u32 primary, u32 extension)
{
    struct sof_ipc4_msg msg = {0};
    msg.primary = primary;
    msg.extension = extension;
    return msg.header_u64;
}
