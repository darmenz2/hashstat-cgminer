/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_UART_LINUX_H
#define HS_AML_UART_LINUX_H

#include "hs_aml_uart.h"

struct hs_aml_uart_linux_identity {
    uint64_t node_device, inode;
    uint32_t rdev_major, rdev_minor;
    bool character;
};

struct hs_aml_uart_linux_syscalls {
    int (*open)(void *, const char *, uint32_t flags, int *);
    int (*stat_fd)(void *, int, struct hs_aml_uart_linux_identity *, int *);
    int (*get_status_flags)(void *, int, int *);
    int (*get_descriptor_flags)(void *, int, int *);
    int (*control)(void *, int, uint32_t, void *, int *);
    ptrdiff_t (*read)(void *, int, uint8_t *, size_t, int *);
    ptrdiff_t (*write)(void *, int, const uint8_t *, size_t, int *);
    int (*poll)(void *, int, uint16_t requested, unsigned timeout_ms,
                uint16_t *returned, int *);
    int (*close)(void *, int, int *);
    int (*monotonic)(void *, int64_t *seconds, int32_t *nanoseconds, int *);
};

struct hs_aml_uart_linux {
    const struct hs_aml_uart_linux *self;
    struct hs_aml_uart_linux_syscalls sys;
    void *sys_context;
    bool (*is_cancelled)(void *);
    void *cancel_context;
    struct hs_aml_uart_linux_identity expected, opened;
    int fd;
    unsigned chain;
    bool verified, identity_lost, close_uncertain;

    int last_errno, close_errno;
};
#define HS_AML_UART_LINUX_INIT { .fd = -1 }

bool hs_aml_uart_linux_init(struct hs_aml_uart_linux *, unsigned chain,
    const struct hs_aml_uart_linux_identity *, bool (*cancelled)(void *), void *);

bool hs_aml_uart_linux_init_with_syscalls(struct hs_aml_uart_linux *, unsigned,
    const struct hs_aml_uart_linux_identity *, const struct hs_aml_uart_linux_syscalls *,
    void *sys_context, bool (*cancelled)(void *), void *cancel_context);
const struct hs_aml_uart_ops *hs_aml_uart_linux_ops(void);

#endif
