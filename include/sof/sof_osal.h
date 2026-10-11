/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright 2026 The SOF Authors
 */
/*
 * Windows Operating System Abstraction Layer (OSAL) for Sound Open Firmware (SOF)
 * Bridges Linux kernel primitives and idioms to Windows Kernel Mode (KMDF/WDM)
 */

#ifndef __SOF_OSAL_H__
#define __SOF_OSAL_H__

#if defined(_MSC_VER)
/* Disable common benign C extensions used by SOF and Linux */
#pragma warning(disable: 4200) /* zero-sized array in struct/union */
#pragma warning(disable: 4201) /* nameless struct/union */
#pragma warning(disable: 4214) /* nonstandard extension: bit field types other than int */
#pragma warning(disable: 4116) /* unnamed type definition in parentheses */
#pragma warning(disable: 4820) /* padding added after data member */
#endif

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#ifndef _STDINT
#ifndef _STDINT_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
#define _STDINT
#define _STDINT_H
#endif
#endif
#ifndef __cplusplus
#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
typedef BOOLEAN bool;
#ifndef true
#define true TRUE
#endif
#ifndef false
#define false FALSE
#endif
#endif
#endif
#else
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _NTSTATUS_DEFINED
typedef LONG NTSTATUS;
#define _NTSTATUS_DEFINED
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND                ((NTSTATUS)0xC0000225L)
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef _PHYSICAL_ADDRESS_DEFINED
typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;
#define _PHYSICAL_ADDRESS_DEFINED
#endif
#endif

#include <intrin.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Basic Integer & Endian Types
 * ========================================================================= */
#ifndef _LINUX_TYPES_H
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef signed char        s8;
typedef signed short       s16;
typedef signed int         s32;
typedef signed long long   s64;

typedef unsigned char      __u8;
typedef unsigned short     __u16;
typedef unsigned int       __u32;
typedef unsigned long long __u64;

typedef signed char        __s8;
typedef signed short       __s16;
typedef signed int         __s32;
typedef signed long long   __s64;

typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;

typedef u64 dma_addr_t;
typedef size_t resource_size_t;
#endif

/* Alignment and Packing attributes */
#ifndef __packed
#define __packed
#endif

#ifndef __aligned
#define __aligned(x) __declspec(align(x))
#endif

#ifndef DECLARE_FLEX_ARRAY
#define DECLARE_FLEX_ARRAY(TYPE, NAME) TYPE NAME[]
#endif

/* =========================================================================
 * Bit manipulation macros & intrinsics
 * ========================================================================= */
#ifndef BIT
#define BIT(nr) (1ULL << (nr))
#endif

#ifndef GENMASK
#define GENMASK(h, l) \
    (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (64 - 1 - (h))))
#endif

#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) GENMASK(h, l)
#endif

static inline unsigned int __sof_ctz64(unsigned long long mask) {
    unsigned long index = 0;
    if (_BitScanForward64(&index, mask)) {
        return index;
    }
    return 0;
}

#ifndef FIELD_PREP
#define FIELD_PREP(mask, val) (((val) << (__sof_ctz64(mask))) & (mask))
#endif

#ifndef FIELD_GET
#define FIELD_GET(mask, val) (((val) & (mask)) >> (__sof_ctz64(mask)))
#endif

/* Array size */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* =========================================================================
 * Error Codes (Mapping Linux errno to NTSTATUS / standard negatives)
 * ========================================================================= */
#ifndef EINVAL
#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define ENODEV          19
#define EINVAL          22
#define ENOSPC          28
#define ETIMEDOUT       110
#endif

/* =========================================================================
 * Synchronization Primitives: Spinlocks, Mutexes, Events/Completions
 * ========================================================================= */

#ifdef _KERNEL_MODE

/* Spinlock */
typedef struct {
    KSPIN_LOCK lock;
    KIRQL irql;
} spinlock_t;

static inline void spin_lock_init(spinlock_t *sl) {
    KeInitializeSpinLock(&sl->lock);
}

static inline void spin_lock(spinlock_t *sl) {
    KeAcquireSpinLock(&sl->lock, &sl->irql);
}

static inline void spin_unlock(spinlock_t *sl) {
    KeReleaseSpinLock(&sl->lock, sl->irql);
}

#define spin_lock_irqsave(sl, flags) do { (flags) = 0; spin_lock(sl); } while(0)
#define spin_unlock_irqrestore(sl, flags) do { (void)(flags); spin_unlock(sl); } while(0)

/* Mutex */
struct mutex {
    FAST_MUTEX mtx;
};

static inline void mutex_init(struct mutex *m) {
    ExInitializeFastMutex(&m->mtx);
}

static inline void mutex_lock(struct mutex *m) {
    ExAcquireFastMutex(&m->mtx);
}

static inline void mutex_unlock(struct mutex *m) {
    ExReleaseFastMutex(&m->mtx);
}

/* Completion */
struct completion {
    KEVENT ev;
};

static inline void init_completion(struct completion *c) {
    KeInitializeEvent(&c->ev, NotificationEvent, FALSE);
}

static inline void complete(struct completion *c) {
    KeSetEvent(&c->ev, 0, FALSE);
}

static inline long wait_for_completion_timeout(struct completion *c, unsigned long timeout_ms) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = -10000LL * (LONGLONG)timeout_ms; // 100ns units, negative = relative
    NTSTATUS st = KeWaitForSingleObject(&c->ev, Executive, KernelMode, FALSE, &timeout);
    if (st == STATUS_SUCCESS) {
        KeResetEvent(&c->ev);
        return 1; // Success
    }
    return 0; // Timed out
}

/* Memory allocation */
#define SOF_POOL_TAG 'FOSW' // 'WSOF'

static inline void *kmalloc(size_t size, int flags) {
    (void)flags;
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, SOF_POOL_TAG);
}

static inline void *kzalloc(size_t size, int flags) {
    (void)flags;
    void *p = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, SOF_POOL_TAG);
    if (p) RtlZeroMemory(p, size);
    return p;
}

static inline void *kcalloc(size_t n, size_t size, int flags) {
    return kzalloc(n * size, flags);
}

static inline void kfree(const void *ptr) {
    if (ptr) ExFreePoolWithTag((PVOID)ptr, SOF_POOL_TAG);
}

/* Delay & Sleep */
static inline void mdelay(unsigned long ms) {
    LARGE_INTEGER interval;
    interval.QuadPart = -10000LL * (LONGLONG)ms;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static inline void udelay(unsigned long us) {
    KeStallExecutionProcessor(us);
}

static inline void usleep_range(unsigned long min_us, unsigned long max_us) {
    (void)max_us;
    udelay(min_us);
}

/* Hardware I/O Register Access */
static inline u32 readl(const volatile void *addr) {
    return READ_REGISTER_ULONG((volatile ULONG *)addr);
}

static inline void writel(u32 val, volatile void *addr) {
    WRITE_REGISTER_ULONG((volatile ULONG *)addr, val);
}

static inline u16 readw(const volatile void *addr) {
    return READ_REGISTER_USHORT((volatile USHORT *)addr);
}

static inline void writew(u16 val, volatile void *addr) {
    WRITE_REGISTER_USHORT((volatile USHORT *)addr, val);
}

static inline u8 readb(const volatile void *addr) {
    return READ_REGISTER_UCHAR((volatile UCHAR *)addr);
}

static inline void writeb(u8 val, volatile void *addr) {
    WRITE_REGISTER_UCHAR((volatile UCHAR *)addr, val);
}

/* Logging */
#define dev_err(dev, fmt, ...)  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "[SOF ERR] " fmt "\n", ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, "[SOF WRN] " fmt "\n", ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "[SOF INF] " fmt "\n", ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "[SOF DBG] " fmt "\n", ##__VA_ARGS__)

#else /* User-mode prototype / test harness */

typedef CRITICAL_SECTION spinlock_t;
static inline void spin_lock_init(spinlock_t *sl) { InitializeCriticalSection(sl); }
static inline void spin_lock(spinlock_t *sl) { EnterCriticalSection(sl); }
static inline void spin_unlock(spinlock_t *sl) { LeaveCriticalSection(sl); }
#define spin_lock_irqsave(sl, flags) do { (flags) = 0; spin_lock(sl); } while(0)
#define spin_unlock_irqrestore(sl, flags) do { (void)(flags); spin_unlock(sl); } while(0)

struct mutex { CRITICAL_SECTION cs; };
static inline void mutex_init(struct mutex *m) { InitializeCriticalSection(&m->cs); }
static inline void mutex_lock(struct mutex *m) { EnterCriticalSection(&m->cs); }
static inline void mutex_unlock(struct mutex *m) { LeaveCriticalSection(&m->cs); }

struct completion { HANDLE ev; };
static inline void init_completion(struct completion *c) { c->ev = CreateEvent(NULL, FALSE, FALSE, NULL); }
static inline void complete(struct completion *c) { SetEvent(c->ev); }
static inline long wait_for_completion_timeout(struct completion *c, unsigned long timeout_ms) {
    return (WaitForSingleObject(c->ev, timeout_ms) == WAIT_OBJECT_0) ? 1 : 0;
}

static inline void *kmalloc(size_t size, int flags) { (void)flags; return malloc(size); }
static inline void *kzalloc(size_t size, int flags) { (void)flags; return calloc(1, size); }
static inline void *kcalloc(size_t n, size_t size, int flags) { (void)flags; return calloc(n, size); }
static inline void kfree(const void *ptr) { free((void*)ptr); }

static inline void mdelay(unsigned long ms) { Sleep(ms); }
static inline void udelay(unsigned long us) { (void)us; Sleep(1); }
static inline void usleep_range(unsigned long min_us, unsigned long max_us) { (void)max_us; udelay(min_us); }

static inline u32 readl(const volatile void *addr) { return *(const volatile u32 *)addr; }
static inline void writel(u32 val, volatile void *addr) { *(volatile u32 *)addr = val; }
static inline u16 readw(const volatile void *addr) { return *(const volatile u16 *)addr; }
static inline void writew(u16 val, volatile void *addr) { *(volatile u16 *)addr = val; }
static inline u8 readb(const volatile void *addr) { return *(const volatile u8 *)addr; }
static inline void writeb(u8 val, volatile void *addr) { *(volatile u8 *)addr = val; }

#define dev_err(dev, fmt, ...)  printf("[SOF ERR] " fmt "\n", ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) printf("[SOF WRN] " fmt "\n", ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) printf("[SOF INF] " fmt "\n", ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)  printf("[SOF DBG] " fmt "\n", ##__VA_ARGS__)

#endif /* _KERNEL_MODE */

/* Forward declarations of driver / device contexts */
struct device {
    void *driver_data;
};

static inline void *dev_get_drvdata(const struct device *dev) {
    return dev ? dev->driver_data : NULL;
}

static inline void dev_set_drvdata(struct device *dev, void *data) {
    if (dev) dev->driver_data = data;
}

#ifdef __cplusplus
}
#endif

#endif /* __SOF_OSAL_H__ */
