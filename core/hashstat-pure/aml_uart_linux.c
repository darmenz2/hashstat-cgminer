/* SPDX-License-Identifier: GPL-3.0-only */

#define _GNU_SOURCE 1
#include "hs_aml_uart_linux.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

#define ARM_O_ACCMODE UINT32_C(3)
#define ARM_O_RDWR UINT32_C(2)
#define ARM_O_NONBLOCK UINT32_C(0x800)
#define ARM_O_APPEND UINT32_C(0x400)
#define ARM_O_ASYNC UINT32_C(0x2000)
#define ARM_FD_CLOEXEC UINT32_C(1)
#define LINUX_POLLIN UINT16_C(1)
#define LINUX_POLLOUT UINT16_C(4)
#define LINUX_POLLERR UINT16_C(8)
#define LINUX_POLLHUP UINT16_C(16)
#define LINUX_POLLNVAL UINT16_C(32)

static bool valid(const struct hs_aml_uart_linux *a)
{
    return a != NULL && a->self == a;
}

static bool same(const struct hs_aml_uart_linux_identity *a,
                 const struct hs_aml_uart_linux_identity *b)
{
    return a->node_device == b->node_device && a->inode == b->inode &&
        a->rdev_major == b->rdev_major && a->rdev_minor == b->rdev_minor &&
        a->character == b->character;
}

static int failure(struct hs_aml_uart_linux *a, int code,
                    enum hs_aml_uart_io_error *error)
{
    if (code <= 0) code = EIO;
    if (valid(a)) a->last_errno = code;
    if (error != NULL) {
        *error = code == EINTR ? HS_UART_IO_INTERRUPTED :
            (code == EAGAIN || code == EWOULDBLOCK ? HS_UART_IO_AGAIN : HS_UART_IO_OTHER);
    }
    return -1;
}

static bool owned(struct hs_aml_uart_linux *a, int fd)
{
    return valid(a) && fd >= 0 && a->fd == fd && !a->identity_lost;
}

static int identity(struct hs_aml_uart_linux *a, int fd,
                    enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux_identity observed = {0};
    int code = 0;
    if (!owned(a, fd)) return failure(a, EBADF, error);
    const int rc = a->sys.stat_fd(a->sys_context, fd, &observed, &code);
    if (rc != 0) return failure(a, rc == -1 ? code : EIO, error);
    if (!same(&observed, &a->opened)) {
        a->identity_lost = true;
        a->fd = -1;
        a->verified = false;
        return failure(a, ESTALE, error);
    }
    return 0;
}

static int flags(struct hs_aml_uart_linux *a, int fd,
                 enum hs_aml_uart_io_error *error)
{
    int code = 0;
    const int fl = a->sys.get_status_flags(a->sys_context, fd, &code);
    if (fl < 0) return failure(a, fl == -1 ? code : EIO, error);
    const uint32_t actual = (uint32_t)fl;
    if ((actual & (ARM_O_ACCMODE | ARM_O_NONBLOCK)) != (ARM_O_RDWR | ARM_O_NONBLOCK) ||
        (actual & (ARM_O_APPEND | ARM_O_ASYNC)) != 0U)
        return failure(a, EPERM, error);
    code = 0;
    const int fdfl = a->sys.get_descriptor_flags(a->sys_context, fd, &code);
    if (fdfl < 0) return failure(a, fdfl == -1 ? code : EIO, error);
    if (((uint32_t)fdfl & ARM_FD_CLOEXEC) == 0U) return failure(a, EPERM, error);
    return 0;
}

static int operational(struct hs_aml_uart_linux *a, int fd,
                       enum hs_aml_uart_io_error *error)
{
    if (identity(a, fd, error) != 0) return -1;
    if (!a->verified || !a->opened.character || !same(&a->opened, &a->expected))
        return failure(a, EPERM, error);
    return flags(a, fd, error);
}

static int adapter_open(void *context, const char *path, uint32_t open_flags,
                        enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux *a = context;
    int code = 0;
    if (!valid(a) || a->fd != -1 || a->identity_lost || a->close_uncertain ||
        path == NULL || open_flags != HS_AML_UART_OPEN_FLAGS ||
        strcmp(path, hs_aml_uart_path(a->chain)) != 0)
        return failure(a, EINVAL, error);
    const int fd = a->sys.open(a->sys_context, path, open_flags, &code);
    if (fd < 0) return failure(a, fd == -1 ? code : EIO, error);
    a->fd = fd;
    a->verified = false;
    memset(&a->opened, 0, sizeof(a->opened));
    int rc = -1;

    for (unsigned i = 0; i < 4U; ++i) {
        code = 0;
        rc = a->sys.stat_fd(a->sys_context, fd, &a->opened, &code);
        if (rc != -1 || code != EINTR) break;
    }
    if (rc != 0) {
        const int primary = rc == -1 ? code : EIO;
        a->fd = -1;
        code = 0;
        const int close_rc = a->sys.close(a->sys_context, fd, &code);
        if (close_rc != 0) {
            a->close_uncertain = true;
            a->close_errno = close_rc == -1 && code > 0 ? code : EIO;
        }

        a->last_errno = primary > 0 ? primary : EIO;
        if (error != NULL) *error = HS_UART_IO_OTHER;
        return -1;
    }
    if (error != NULL) *error = HS_UART_IO_NONE;
    return fd;
}

static int adapter_verify(void *context, int fd, unsigned chain,
                          enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux *a = context;
    if (identity(a, fd, error) != 0) return -1;
    if (chain != a->chain || !a->opened.character || !same(&a->opened, &a->expected))
        return failure(a, EPERM, error);
    if (flags(a, fd, error) != 0) return -1;
    a->verified = true;
    if (error != NULL) *error = HS_UART_IO_NONE;
    return 0;
}

static int adapter_control(void *context, int fd, uint32_t request, void *arg,
                           enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux *a = context;
    if ((request != HS_AML_UART_TCGETS2 && request != HS_AML_UART_TCSETS2 &&
         request != HS_AML_UART_TIOCEXCL && request != HS_AML_UART_TIOCNXCL) ||
        ((request == HS_AML_UART_TCGETS2 || request == HS_AML_UART_TCSETS2) ?
            arg == NULL : arg != NULL)) return failure(a, EINVAL, error);

    if (identity(a, fd, error) != 0) return -1;
    if (!a->verified || !same(&a->opened, &a->expected)) return failure(a, EPERM, error);
    int code = 0;
    const int rc = a->sys.control(a->sys_context, fd, request, arg, &code);
    if (rc != 0) return failure(a, rc == -1 ? code : EIO, error);
    if (error != NULL) *error = HS_UART_IO_NONE;
    return 0;
}

static ptrdiff_t adapter_read(void *context, int fd, uint8_t *buffer, size_t n,
                              enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux *a = context;
    if (buffer == NULL || n == 0U || n > HS_AML_UART_MAX_BYTES)
        return failure(a, EINVAL, error);
    if (operational(a, fd, error) != 0) return -1;
    int code = 0;
    const ptrdiff_t rc = a->sys.read(a->sys_context, fd, buffer, n, &code);
    if (rc < 0) return failure(a, rc == -1 ? code : EIO, error);
    if ((size_t)rc > n) return failure(a, EIO, error);
    if (error != NULL) *error = HS_UART_IO_NONE;
    return rc;
}

static ptrdiff_t adapter_write(void *context, int fd, const uint8_t *buffer, size_t n,
                               enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux *a = context;
    if (buffer == NULL || n == 0U || n > HS_AML_UART_MAX_BYTES)
        return failure(a, EINVAL, error);
    if (operational(a, fd, error) != 0) return -1;
    int code = 0;
    const ptrdiff_t rc = a->sys.write(a->sys_context, fd, buffer, n, &code);
    if (rc < 0) return failure(a, rc == -1 ? code : EIO, error);
    if ((size_t)rc > n) return failure(a, EIO, error);
    if (error != NULL) *error = HS_UART_IO_NONE;
    return rc;
}

static int adapter_wait(void *context, int fd, unsigned requested, unsigned timeout_ms,
                        unsigned *events, enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux *a = context;
    if (events == NULL || (requested != HS_UART_READABLE && requested != HS_UART_WRITABLE) ||
        timeout_ms == 0U || timeout_ms > HS_AML_UART_POLL_SLICE_MS)
        return failure(a, EINVAL, error);
    *events = 0U;
    if (operational(a, fd, error) != 0) return -1;
    uint16_t observed = 0U;
    int code = 0;
    const int rc = a->sys.poll(a->sys_context, fd,
        requested == HS_UART_READABLE ? LINUX_POLLIN : LINUX_POLLOUT,
        timeout_ms, &observed, &code);
    if (rc < 0) return failure(a, rc == -1 ? code : EIO, error);
    if (rc > 1 || (rc == 0 && observed != 0U) || (rc == 1 && observed == 0U) ||
        (observed & (uint16_t)~(LINUX_POLLIN | LINUX_POLLOUT | LINUX_POLLERR |
                               LINUX_POLLHUP | LINUX_POLLNVAL)) != 0U)
        return failure(a, EIO, error);
    if ((observed & LINUX_POLLIN) != 0U) *events |= HS_UART_READABLE;
    if ((observed & LINUX_POLLOUT) != 0U) *events |= HS_UART_WRITABLE;
    if ((observed & LINUX_POLLERR) != 0U) *events |= HS_UART_ERROR;
    if ((observed & LINUX_POLLHUP) != 0U) *events |= HS_UART_HUP;
    if ((observed & LINUX_POLLNVAL) != 0U) *events |= HS_UART_INVALID_FD;
    if (error != NULL) *error = HS_UART_IO_NONE;
    return rc;
}

static int adapter_close(void *context, int fd, enum hs_aml_uart_io_error *error)
{
    struct hs_aml_uart_linux *a = context;
    if (!owned(a, fd)) return failure(a, EBADF, error);
    int check = -1;
    enum hs_aml_uart_io_error checked_error = HS_UART_IO_NONE;
    for (unsigned i = 0; i < 4U; ++i) {
        check = identity(a, fd, &checked_error);
        if (check == 0 || checked_error != HS_UART_IO_INTERRUPTED) break;
    }
    if (check != 0) {
        a->fd = -1;
        a->verified = false;
        a->close_uncertain = true;
        a->close_errno = a->last_errno;
        if (error != NULL) *error = checked_error;
        return -1;
    }
    a->fd = -1;
    a->verified = false;
    int code = 0;
    const int rc = a->sys.close(a->sys_context, fd, &code);
    if (rc != 0) {
        a->close_uncertain = true;
        a->close_errno = rc == -1 && code > 0 ? code : EIO;
        return failure(a, rc == -1 ? code : EIO, error);
    }
    if (error != NULL) *error = HS_UART_IO_NONE;
    return 0;
}

static bool adapter_now(void *context, uint64_t *milliseconds)
{
    struct hs_aml_uart_linux *a = context;
    int64_t seconds = 0;
    int32_t nanoseconds = 0;
    int code = 0;
    if (!valid(a) || milliseconds == NULL) return false;
    const int rc = a->sys.monotonic(a->sys_context, &seconds, &nanoseconds, &code);
    if (rc != 0) { a->last_errno = rc == -1 && code > 0 ? code : EIO; return false; }
    if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000 ||
        (uint64_t)seconds > (UINT64_MAX - (uint32_t)nanoseconds / UINT32_C(1000000)) / UINT64_C(1000))
        { a->last_errno = EOVERFLOW; return false; }
    *milliseconds = (uint64_t)seconds * UINT64_C(1000) + (uint32_t)nanoseconds / UINT32_C(1000000);
    return true;
}

static bool adapter_cancelled(void *context)
{
    struct hs_aml_uart_linux *a = context;
    return !valid(a) || (a->is_cancelled != NULL && a->is_cancelled(a->cancel_context));
}

static const struct hs_aml_uart_ops adapter_ops = {
    adapter_open, adapter_verify, adapter_control, adapter_read, adapter_write,
    adapter_wait, adapter_close, adapter_now, adapter_cancelled
};

const struct hs_aml_uart_ops *hs_aml_uart_linux_ops(void) { return &adapter_ops; }

bool hs_aml_uart_linux_init_with_syscalls(struct hs_aml_uart_linux *a, unsigned chain,
    const struct hs_aml_uart_linux_identity *expected, const struct hs_aml_uart_linux_syscalls *s,
    void *sys_context, bool (*cancelled)(void *), void *cancel_context)
{
    if (a == NULL || a->fd != -1 || (a->self != NULL && a->self != a) || chain >= 3U ||
        expected == NULL || !expected->character || expected->inode == 0U ||
        s == NULL || s->open == NULL || s->stat_fd == NULL || s->get_status_flags == NULL ||
        s->get_descriptor_flags == NULL || s->control == NULL || s->read == NULL ||
        s->write == NULL || s->poll == NULL || s->close == NULL || s->monotonic == NULL)
        return false;
    const struct hs_aml_uart_linux_identity pins = *expected;
    const struct hs_aml_uart_linux_syscalls calls = *s;
    memset(a, 0, sizeof(*a));
    a->self = a;
    a->fd = -1;
    a->chain = chain;
    a->expected = pins;
    a->sys = calls;
    a->sys_context = sys_context;
    a->is_cancelled = cancelled;
    a->cancel_context = cancel_context;
    return true;
}

#if defined(__linux__) && defined(__arm__) && !defined(__aarch64__) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <asm/termbits.h>

_Static_assert(sizeof(void *) == 4U, "ARM32 userspace required");
_Static_assert(sizeof(struct termios2) == sizeof(struct hs_aml_uart_termios2), "termios2 ABI");
#define TERM_FIELD(native, local) \
    _Static_assert(offsetof(struct termios2, native) == \
                   offsetof(struct hs_aml_uart_termios2, local), "termios2 " #native)
TERM_FIELD(c_iflag, iflag);
TERM_FIELD(c_oflag, oflag);
TERM_FIELD(c_cflag, cflag);
TERM_FIELD(c_lflag, lflag);
TERM_FIELD(c_line, line);
TERM_FIELD(c_cc, cc);
TERM_FIELD(c_ispeed, ispeed);
TERM_FIELD(c_ospeed, ospeed);
#undef TERM_FIELD
_Static_assert(NCCS == 19 && VTIME == 5 && VMIN == 6, "termios2 control characters");
_Static_assert((O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW) == HS_AML_UART_OPEN_FLAGS, "ARM flags");

_Static_assert((O_ACCMODE & ~O_PATH) == ARM_O_ACCMODE && O_RDWR == ARM_O_RDWR && O_NONBLOCK == ARM_O_NONBLOCK &&
    O_APPEND == ARM_O_APPEND && O_ASYNC == ARM_O_ASYNC && FD_CLOEXEC == ARM_FD_CLOEXEC, "ARM fcntl");
_Static_assert(POLLIN == LINUX_POLLIN && POLLOUT == LINUX_POLLOUT && POLLERR == LINUX_POLLERR &&
    POLLHUP == LINUX_POLLHUP && POLLNVAL == LINUX_POLLNVAL, "Linux poll ABI");

_Static_assert(_IOR('T', 0x2a, struct termios2) == HS_AML_UART_TCGETS2 &&
    _IOW('T', 0x2b, struct termios2) == HS_AML_UART_TCSETS2 &&
    TIOCEXCL == HS_AML_UART_TIOCEXCL && TIOCNXCL == HS_AML_UART_TIOCNXCL, "ioctl ABI");

static int native_open(void *c, const char *path, uint32_t fl, int *e)
{ (void)c; const int r = open(path, (int)fl); if (r == -1) *e = errno; return r; }
static int native_stat(void *c, int fd, struct hs_aml_uart_linux_identity *out, int *e)
{
    (void)c; struct stat s;
    const int r = fstat(fd, &s);
    if (r == -1) { *e = errno; return -1; }
    *out = (struct hs_aml_uart_linux_identity){(uint64_t)s.st_dev, (uint64_t)s.st_ino,
        (uint32_t)major(s.st_rdev), (uint32_t)minor(s.st_rdev), S_ISCHR(s.st_mode)};
    return 0;
}
static int native_fl(void *c, int fd, int *e)
{ (void)c; const int r = fcntl(fd, F_GETFL); if (r == -1) *e = errno; return r; }
static int native_fdfl(void *c, int fd, int *e)
{ (void)c; const int r = fcntl(fd, F_GETFD); if (r == -1) *e = errno; return r; }
static int native_control(void *c, int fd, uint32_t req, void *arg, int *e)
{
    (void)c;

#if defined(__GLIBC__)
    const int r = ioctl(fd, (unsigned long)req, arg);
#else
    const int r = ioctl(fd, (int)req, arg);
#endif
    if (r == -1) *e = errno;
    return r;
}
static ptrdiff_t native_read(void *c, int fd, uint8_t *b, size_t n, int *e)
{ (void)c; const ssize_t r = read(fd, b, n); if (r == -1) *e = errno; return (ptrdiff_t)r; }
static ptrdiff_t native_write(void *c, int fd, const uint8_t *b, size_t n, int *e)
{ (void)c; const ssize_t r = write(fd, b, n); if (r == -1) *e = errno; return (ptrdiff_t)r; }
static int native_poll(void *c, int fd, uint16_t requested, unsigned timeout,
                       uint16_t *out, int *e)
{
    (void)c; struct pollfd p = {fd, (short)requested, 0};
    const int r = poll(&p, 1, (int)timeout);
    if (r == -1) { *e = errno; return -1; }
    *out = (uint16_t)p.revents;
    return r;
}
static int native_close(void *c, int fd, int *e)
{ (void)c; const int r = close(fd); if (r == -1) *e = errno; return r; }
static int native_monotonic(void *c, int64_t *sec, int32_t *ns, int *e)
{
    (void)c; struct timespec t;
    const int r = clock_gettime(CLOCK_MONOTONIC, &t);
    if (r == -1) { *e = errno; return -1; }
    *sec = (int64_t)t.tv_sec;
    *ns = (int32_t)t.tv_nsec;
    return 0;
}
static const struct hs_aml_uart_linux_syscalls native_calls = {
    native_open, native_stat, native_fl, native_fdfl, native_control, native_read,
    native_write, native_poll, native_close, native_monotonic
};
#endif

bool hs_aml_uart_linux_init(struct hs_aml_uart_linux *a, unsigned chain,
    const struct hs_aml_uart_linux_identity *pins, bool (*cancelled)(void *), void *context)
{
#if defined(__linux__) && defined(__arm__) && !defined(__aarch64__) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return hs_aml_uart_linux_init_with_syscalls(a, chain, pins, &native_calls, NULL,
                                               cancelled, context);
#else
    (void)a; (void)chain; (void)pins; (void)cancelled; (void)context;
    return false;
#endif
}
