/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#ifndef _COMPAT_LINUX_UUID_H
#define _COMPAT_LINUX_UUID_H

#include <sof/sof_osal.h>

typedef struct {
    u8 b[16];
} guid_t;

typedef struct {
    u8 b[16];
} sof_uuid_t;

#endif
