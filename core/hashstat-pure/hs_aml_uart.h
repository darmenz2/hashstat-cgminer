/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_UART_H
#define HS_AML_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HS_AML_UART_TCGETS2 UINT32_C(0x802c542a)
#define HS_AML_UART_TCSETS2 UINT32_C(0x402c542b)
#define HS_AML_UART_TIOCEXCL UINT32_C(0x540c)
#define HS_AML_UART_TIOCNXCL UINT32_C(0x540d)
#define HS_AML_UART_OPEN_FLAGS UINT32_C(0x00088902)
#define HS_AML_UART_MAX_BYTES ((size_t)65536)
#define HS_AML_UART_MAX_TIMEOUT_MS UINT32_C(30000)
#define HS_AML_UART_MAX_ATTEMPTS ((unsigned)16384)
#define HS_AML_UART_POLL_SLICE_MS ((unsigned)20)

struct hs_aml_uart_termios2 {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
    uint32_t ispeed, ospeed;
};

enum hs_aml_uart_io_error {
    HS_UART_IO_NONE = 0, HS_UART_IO_INTERRUPTED, HS_UART_IO_AGAIN,
    HS_UART_IO_OTHER
};
enum hs_aml_uart_status {
    HS_UART_OK = 0, HS_UART_ARGUMENT, HS_UART_NOT_READY, HS_UART_NOT_OPEN,
    HS_UART_CANCELLED, HS_UART_TIMEOUT, HS_UART_CLOCK_ERROR, HS_UART_BUDGET,
    HS_UART_IO_ERROR, HS_UART_CALLBACK_ERROR, HS_UART_HANGUP,
    HS_UART_CONFIG_UNSUPPORTED, HS_UART_CONFIG_MISMATCH
};
enum hs_aml_uart_event {
    HS_UART_READABLE = 1, HS_UART_WRITABLE = 2, HS_UART_ERROR = 4,
    HS_UART_HUP = 8, HS_UART_INVALID_FD = 16
};
enum hs_aml_uart_cleanup_error {
    HS_UART_RESTORE_FAILED = 1, HS_UART_CLOSE_FAILED = 2,
    HS_UART_EXCLUSIVE_RELEASE_FAILED = 4
};

struct hs_aml_uart_ops {
    int (*open)(void *, const char *, uint32_t flags, enum hs_aml_uart_io_error *);
    int (*verify_fd)(void *, int fd, unsigned chain, enum hs_aml_uart_io_error *);
    int (*control)(void *, int fd, uint32_t request, void *arg,
                   enum hs_aml_uart_io_error *);
    ptrdiff_t (*read)(void *, int fd, uint8_t *, size_t, enum hs_aml_uart_io_error *);
    ptrdiff_t (*write)(void *, int fd, const uint8_t *, size_t,
                       enum hs_aml_uart_io_error *);
    int (*wait)(void *, int fd, unsigned requested, unsigned timeout_ms,
                unsigned *events, enum hs_aml_uart_io_error *);
    int (*close)(void *, int fd, enum hs_aml_uart_io_error *);
    bool (*now_ms)(void *, uint64_t *);
    bool (*cancelled)(void *);
};
struct hs_aml_uart_readiness {
    bool explicit_uart_opt_in;
    bool aml_kernel_binding_verified;
    bool character_device_verified;
    bool previous_owner_closed;
    bool byte_writes_authorized;
};
struct hs_aml_uart_result {
    enum hs_aml_uart_status status;

    size_t transferred;
    unsigned cleanup_errors;
};

struct hs_aml_uart_session {
    int fd;
    bool active, original_valid, restore_needed, exclusive_attempted, writes_authorized;
    unsigned chain;
    struct hs_aml_uart_termios2 original;
    struct hs_aml_uart_ops ops;
    void *context;
};
#define HS_AML_UART_SESSION_INIT { .fd = -1 }

const char *hs_aml_uart_path(unsigned chain);
bool hs_aml_uart_make_profile(const struct hs_aml_uart_termios2 *original,
                              struct hs_aml_uart_termios2 *configured);

struct hs_aml_uart_result hs_aml_uart_open(
    struct hs_aml_uart_session *, const struct hs_aml_uart_ops *, void *context,
    unsigned chain, struct hs_aml_uart_readiness, uint32_t timeout_ms);
struct hs_aml_uart_result hs_aml_uart_read_some(
    struct hs_aml_uart_session *, uint8_t *, size_t capacity, uint32_t timeout_ms);
struct hs_aml_uart_result hs_aml_uart_write_all(
    struct hs_aml_uart_session *, const uint8_t *, size_t length, uint32_t timeout_ms);

unsigned hs_aml_uart_close(struct hs_aml_uart_session *);

#endif
