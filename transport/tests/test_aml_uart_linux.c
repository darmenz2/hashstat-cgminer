/* SPDX-License-Identifier: GPL-3.0-only */


#include "hs_aml_uart_linux.h"
#ifdef NDEBUG
#error "These executable tests require assertions enabled"
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum operation { OPEN, STAT, FLAGS, FDFLAGS, EXCL, GET, SET, RESTORE, RELEASE,
                 READ, WRITE, POLL, CLOSE, CLOCK, OP_COUNT };
struct fault { int rc, error; unsigned skip, remaining; };
struct action { ptrdiff_t rc; int error; };
struct mock {
    struct hs_aml_uart_linux *adapter;
    struct hs_aml_uart_linux_identity node;
    struct hs_aml_uart_termios2 original, current;
    struct fault faults[OP_COUNT];
    unsigned calls[OP_COUNT], chain, max_poll, polls_to_cancel;
    unsigned action_count, action_index;
    struct action actions[8];
    uint8_t written[1024];
    size_t written_count;
    int fd, flags, fdflags, poll_rc;
    uint16_t revents;
    uint64_t now;
    int64_t seconds;
    int32_t nanoseconds;
    unsigned clock_mode;
    bool cancel, frozen, mismatch, closed, clear_before_close;
};
static unsigned cases;
static const struct hs_aml_uart_ops *ops;
static const struct hs_aml_uart_linux_identity pins = {12U, 34U, 4U, 67U, true};
static const struct hs_aml_uart_readiness ready = {true, true, true, true, true};

static bool fault(struct mock *m, enum operation op, int *error, int *rc)
{
    struct fault *f = &m->faults[op];
    ++m->calls[op];
    if (f->skip != 0U) { --f->skip; return false; }
    if (f->remaining == 0U) return false;
    --f->remaining;
    *error = f->error;
    *rc = f->rc;
    return true;
}
static void inject(struct mock *m, enum operation op, int rc, int error,
                   unsigned count, unsigned skip)
{ m->faults[op] = (struct fault){rc, error, skip, count}; }
static int mock_open(void *ctx, const char *path, uint32_t flags, int *error)
{
    struct mock *m = ctx; int rc;
    assert(strcmp(path, hs_aml_uart_path(m->chain)) == 0);
    assert(flags == UINT32_C(0x88902));
    if (fault(m, OPEN, error, &rc)) return rc;
    m->closed = false;
    *error = EINTR;
    return m->fd;
}
static int mock_stat(void *ctx, int fd, struct hs_aml_uart_linux_identity *out, int *error)
{
    struct mock *m = ctx; int rc;
    assert(fd == m->fd);
    if (fault(m, STAT, error, &rc)) return rc;
    *out = m->node;
    return 0;
}
static int mock_flags(void *ctx, int fd, int *error)
{
    struct mock *m = ctx; int rc; assert(fd == m->fd);
    return fault(m, FLAGS, error, &rc) ? rc : m->flags;
}
static int mock_fdflags(void *ctx, int fd, int *error)
{
    struct mock *m = ctx; int rc; assert(fd == m->fd);
    return fault(m, FDFLAGS, error, &rc) ? rc : m->fdflags;
}
static int mock_control(void *ctx, int fd, uint32_t request, void *arg, int *error)
{
    struct mock *m = ctx; int rc; enum operation op;
    assert(fd == m->fd);
    if (request == HS_AML_UART_TIOCEXCL || request == HS_AML_UART_TIOCNXCL) {
        assert(arg == NULL);
        op = request == HS_AML_UART_TIOCEXCL ? EXCL : RELEASE;
        return fault(m, op, error, &rc) ? rc : 0;
    }
    assert(arg != NULL);
    if (request == HS_AML_UART_TCGETS2) {
        if (fault(m, GET, error, &rc)) return rc;
        *(struct hs_aml_uart_termios2 *)arg = m->current;
        if (m->mismatch && m->calls[SET] != 0U)
            ((struct hs_aml_uart_termios2 *)arg)->ospeed ^= 1U;
        return 0;
    }
    assert(request == HS_AML_UART_TCSETS2);
    op = memcmp(arg, &m->original, sizeof(m->original)) == 0 ? RESTORE : SET;
    m->current = *(struct hs_aml_uart_termios2 *)arg;
    return fault(m, op, error, &rc) ? rc : 0;
}
static ptrdiff_t io_action(struct mock *m, enum operation op, size_t n, int *error)
{
    int rc;
    if (fault(m, op, error, &rc)) return rc;
    if (m->action_count != 0U) {
        unsigned i = m->action_index < m->action_count ? m->action_index : m->action_count - 1U;
        ++m->action_index;
        *error = m->actions[i].error;
        return m->actions[i].rc;
    }
    *error = EAGAIN;
    return (ptrdiff_t)n;
}
static ptrdiff_t mock_read(void *ctx, int fd, uint8_t *b, size_t n, int *error)
{
    struct mock *m = ctx; assert(fd == m->fd);
    const ptrdiff_t rc = io_action(m, READ, n, error);
    if (rc > 0 && (size_t)rc <= n) memset(b, 0x5a, (size_t)rc);
    return rc;
}
static ptrdiff_t mock_write(void *ctx, int fd, const uint8_t *b, size_t n, int *error)
{
    struct mock *m = ctx; assert(fd == m->fd);
    const ptrdiff_t rc = io_action(m, WRITE, n, error);
    if (rc > 0 && (size_t)rc <= n) {
        assert(m->written_count + (size_t)rc <= sizeof(m->written));
        memcpy(m->written + m->written_count, b, (size_t)rc);
        m->written_count += (size_t)rc;
    }
    return rc;
}
static int mock_poll(void *ctx, int fd, uint16_t requested, unsigned timeout,
                     uint16_t *returned, int *error)
{
    struct mock *m = ctx; int rc;
    assert(fd == m->fd && (requested == 1U || requested == 4U));
    assert(timeout > 0U && timeout <= 20U);
    if (timeout > m->max_poll) m->max_poll = timeout;
    if (!m->frozen) m->now += timeout;
    if (m->polls_to_cancel != 0U && --m->polls_to_cancel == 0U) m->cancel = true;
    if (fault(m, POLL, error, &rc)) return rc;
    *returned = m->revents;
    return m->poll_rc;
}
static int mock_close(void *ctx, int fd, int *error)
{
    struct mock *m = ctx; int rc; assert(fd == m->fd);
    m->clear_before_close = m->adapter->fd == -1;
    m->closed = true;
    return fault(m, CLOSE, error, &rc) ? rc : 0;
}
static int mock_clock(void *ctx, int64_t *sec, int32_t *ns, int *error)
{
    struct mock *m = ctx; int rc;
    if (fault(m, CLOCK, error, &rc)) return rc;
    *sec = m->clock_mode == 0U ? (int64_t)(m->now / 1000U) : m->seconds;
    *ns = m->clock_mode == 0U ? (int32_t)((m->now % 1000U) * 1000000U) : m->nanoseconds;
    return 0;
}
static bool mock_cancel(void *ctx) { return ((struct mock *)ctx)->cancel; }
static const struct hs_aml_uart_linux_syscalls sys = {
    mock_open, mock_stat, mock_flags, mock_fdflags, mock_control,
    mock_read, mock_write, mock_poll, mock_close, mock_clock
};
static void prepare(struct mock *m, struct hs_aml_uart_linux *a, unsigned chain)
{
    memset(m, 0, sizeof(*m));
    *a = (struct hs_aml_uart_linux)HS_AML_UART_LINUX_INIT;
    m->adapter = a; m->node = pins; m->fd = 7; m->chain = chain;
    m->flags = 0x802; m->fdflags = 1; m->poll_rc = 1; m->revents = 5U;
    m->original.iflag = UINT32_MAX; m->original.cflag = UINT32_MAX;
    m->original.ospeed = 9600U; m->original.ispeed = 9600U;
    m->current = m->original;
    assert(hs_aml_uart_linux_init_with_syscalls(a, chain, &pins, &sys, m, mock_cancel, m));
}
static void open_ok(struct mock *m, struct hs_aml_uart_linux *a,
                    struct hs_aml_uart_session *s)
{
    const struct hs_aml_uart_result r = hs_aml_uart_open(s, ops, a, m->chain, ready, 100U);
    assert(r.status == HS_UART_OK && r.cleanup_errors == 0U);
    assert(a->verified && a->fd == m->fd && s->active);
    assert(m->calls[OPEN] == 1U && m->calls[EXCL] == 1U && m->calls[GET] == 2U);
    assert(m->current.ospeed == 115200U && m->current.cc[5] == 0U && m->current.cc[6] == 7U);
}
static void close_ok(struct mock *m, struct hs_aml_uart_linux *a,
                     struct hs_aml_uart_session *s)
{
    assert(hs_aml_uart_close(s) == 0U);
    assert(!s->active && a->fd == -1 && !a->verified);
    assert(m->closed && m->clear_before_close && m->calls[CLOSE] == 1U);
    assert(memcmp(&m->current, &m->original, sizeof(m->original)) == 0);
    assert(hs_aml_uart_close(s) == 0U && m->calls[CLOSE] == 1U);
}

static void test_mapping_lifecycle(void)
{
    for (unsigned chain = 0; chain < 3U; ++chain) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        uint8_t bytes[16]; prepare(&m, &a, chain); open_ok(&m, &a, &s);
        assert(hs_aml_uart_read_some(&s, bytes, sizeof(bytes), 50U).transferred == sizeof(bytes));
        assert(bytes[0] == 0x5a && bytes[15] == 0x5a);
        assert(hs_aml_uart_write_all(&s, bytes, sizeof(bytes), 50U).transferred == sizeof(bytes));
        assert(memcmp(bytes, m.written, sizeof(bytes)) == 0);
        assert(!hs_aml_uart_linux_init_with_syscalls(&a, chain, &pins, &sys, &m, NULL, NULL));
        close_ok(&m, &a, &s); ++cases;
    }
}
static void test_init_guards(void)
{
    struct mock m; struct hs_aml_uart_linux a, copied;
    prepare(&m, &a, 0U); copied = a;
    assert(!hs_aml_uart_linux_init_with_syscalls(NULL, 0U, &pins, &sys, &m, NULL, NULL));
    assert(!hs_aml_uart_linux_init_with_syscalls(&copied, 0U, &pins, &sys, &m, NULL, NULL));
    assert(!hs_aml_uart_linux_init_with_syscalls(&a, 3U, &pins, &sys, &m, NULL, NULL));
    assert(!hs_aml_uart_linux_init_with_syscalls(&a, 0U, NULL, &sys, &m, NULL, NULL));
    assert(!hs_aml_uart_linux_init_with_syscalls(&a, 0U, &pins, NULL, &m, NULL, NULL));
    struct hs_aml_uart_linux_identity bad = pins; bad.character = false;
    assert(!hs_aml_uart_linux_init_with_syscalls(&a, 0U, &bad, &sys, &m, NULL, NULL));
    bad = pins; bad.inode = 0U;
    assert(!hs_aml_uart_linux_init_with_syscalls(&a, 0U, &bad, &sys, &m, NULL, NULL));
    for (unsigned i = 0; i < 10U; ++i) {
        struct hs_aml_uart_linux_syscalls missing = sys;
        switch (i) {
        case 0: missing.open = NULL; break; case 1: missing.stat_fd = NULL; break;
        case 2: missing.get_status_flags = NULL; break; case 3: missing.get_descriptor_flags = NULL; break;
        case 4: missing.control = NULL; break; case 5: missing.read = NULL; break;
        case 6: missing.write = NULL; break; case 7: missing.poll = NULL; break;
        case 8: missing.close = NULL; break; default: missing.monotonic = NULL; break;
        }
        assert(!hs_aml_uart_linux_init_with_syscalls(&a, 0U, &pins, &missing, &m, NULL, NULL));
        ++cases;
    }
#if !defined(__linux__) || !defined(__arm__)
    assert(!hs_aml_uart_linux_init(&a, 0U, &pins, NULL, NULL));
#endif
    enum hs_aml_uart_io_error e = HS_UART_IO_NONE;
    assert(ops->open(&copied, "/dev/ttyS3", HS_AML_UART_OPEN_FLAGS, &e) == -1);
    assert(ops->open(&a, "/dev/ttyS0", HS_AML_UART_OPEN_FLAGS, &e) == -1);
    assert(ops->open(&a, "/dev/ttyS3", 0U, &e) == -1);
    assert(m.calls[OPEN] == 0U);
    ++cases;
}
static void test_bad_pins_flags(void)
{
    for (unsigned i = 0; i < 11U; ++i) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U);
        switch (i) {
        case 0: ++m.node.node_device; break; case 1: ++m.node.inode; break;
        case 2: ++m.node.rdev_major; break; case 3: ++m.node.rdev_minor; break;
        case 4: m.node.character = false; break; case 5: m.flags &= ~0x800; break;
        case 6: m.flags = 0x800; break; case 7: m.flags |= 0x400; break;
        case 8: m.flags |= 0x2000; break; case 9: m.fdflags = 0; break;
        default: m.fdflags = 2; break;
        }
        const struct hs_aml_uart_result r = hs_aml_uart_open(&s, ops, &a, 0U, ready, 100U);
        assert(r.status == HS_UART_NOT_READY && r.cleanup_errors == 0U);
        assert(m.calls[EXCL] == 0U && m.calls[SET] == 0U && m.calls[CLOSE] == 1U);
        assert(a.fd == -1 && !s.active); ++cases;
    }
}
static void test_open_faults(void)
{
    const enum operation stages[] = {OPEN, STAT, FLAGS, FDFLAGS, EXCL, GET, SET};
    for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        for (unsigned mode = 0; mode < 3U; ++mode) {
            struct mock m; struct hs_aml_uart_linux a;
            struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
            prepare(&m, &a, 0U);
            inject(&m, stages[i], -1, mode == 0U ? EINTR : EIO, mode == 2U ? 4U : 1U, 0U);
            const struct hs_aml_uart_result r = hs_aml_uart_open(&s, ops, &a, 0U, ready, 100U);
            if (mode == 0U) {
                assert(r.status == HS_UART_OK);
                close_ok(&m, &a, &s);
            } else {
                assert(r.status != HS_UART_OK && !s.active && a.fd == -1);
                assert(m.calls[CLOSE] == (stages[i] == OPEN ? 0U : 1U));
            }
            ++cases;
        }
    }
    for (unsigned count = 1U; count <= 4U; ++count) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U); inject(&m, STAT, -1, EINTR, count, 0U);
        const struct hs_aml_uart_result r = hs_aml_uart_open(&s, ops, &a, 0U, ready, 100U);
        if (count < 4U) { assert(r.status == HS_UART_OK); close_ok(&m, &a, &s); }
        else { assert(r.status == HS_UART_IO_ERROR && m.calls[STAT] == 4U && m.calls[CLOSE] == 1U); }
        assert(m.calls[OPEN] == 1U); ++cases;
    }
    struct mock m; struct hs_aml_uart_linux a;
    struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
    prepare(&m, &a, 0U); inject(&m, STAT, -1, EIO, 1U, 0U); inject(&m, CLOSE, -1, EINTR, 1U, 0U);
    const struct hs_aml_uart_result r = hs_aml_uart_open(&s, ops, &a, 0U, ready, 100U);
    assert(r.status == HS_UART_IO_ERROR && r.cleanup_errors == 0U);
    assert(a.close_uncertain && a.close_errno == EINTR && a.last_errno == EIO);
    assert(m.calls[CLOSE] == 1U && a.fd == -1);
    assert(hs_aml_uart_open(&s, ops, &a, 0U, ready, 100U).status == HS_UART_IO_ERROR);
    assert(m.calls[OPEN] == 1U); ++cases;
}
static void test_cleanup_and_identity(void)
{
    const enum operation stages[] = {RESTORE, RELEASE, CLOSE};
    const unsigned bits[] = {HS_UART_RESTORE_FAILED, HS_UART_EXCLUSIVE_RELEASE_FAILED, HS_UART_CLOSE_FAILED};
    for (unsigned i = 0; i < 3U; ++i) {
        for (unsigned mode = 0; mode < 3U; ++mode) {
            struct mock m; struct hs_aml_uart_linux a;
            struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
            prepare(&m, &a, 0U); open_ok(&m, &a, &s);
            inject(&m, stages[i], -1, mode == 0U ? EIO : EINTR, mode == 2U ? 4U : 1U, 0U);
            unsigned errors = hs_aml_uart_close(&s);
            assert(errors == ((mode == 1U && stages[i] != CLOSE) ? 0U : bits[i]));
            assert(m.calls[CLOSE] == 1U && m.clear_before_close && a.fd == -1);
            if (stages[i] == CLOSE) assert(a.close_uncertain && a.close_errno == (mode == 0U ? EIO : EINTR));
            assert(hs_aml_uart_close(&s) == 0U); ++cases;
        }
    }
    for (unsigned change = 0; change < 6U; ++change) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        uint8_t b = 1U; prepare(&m, &a, 0U); open_ok(&m, &a, &s);
        switch (change) {
        case 0: ++m.node.inode; break; case 1: ++m.node.node_device; break;
        case 2: ++m.node.rdev_major; break; case 3: ++m.node.rdev_minor; break;
        case 4: m.node.character = false; break;
        default: inject(&m, STAT, -1, EBADF, UINT_MAX, 0U); break;
        }
        struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, &b, 1U, 100U);
        assert(r.status == HS_UART_IO_ERROR && r.transferred == 0U);
        assert(r.cleanup_errors == 7U && !s.active && a.fd == -1);
        assert(m.calls[WRITE] == 0U && m.calls[RESTORE] == 0U && m.calls[RELEASE] == 0U && m.calls[CLOSE] == 0U);
        assert(change == 5U ? a.close_uncertain : a.identity_lost); ++cases;
    }
    struct mock m; struct hs_aml_uart_linux a;
    struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
    uint8_t b = 1U; prepare(&m, &a, 0U); open_ok(&m, &a, &s); m.flags = 2;
    struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, &b, 1U, 100U);
    assert(r.status == HS_UART_IO_ERROR && r.cleanup_errors == 0U && m.calls[WRITE] == 0U);
    assert(m.calls[RESTORE] == 1U && m.calls[RELEASE] == 1U && m.calls[CLOSE] == 1U); ++cases;
}
static void test_partial_and_errors(void)
{
    uint8_t bytes[256]; for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
    for (unsigned prefix = 1U; prefix < sizeof(bytes); ++prefix) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U); open_ok(&m, &a, &s);
        m.actions[0] = (struct action){(ptrdiff_t)prefix, EAGAIN};
        m.actions[1] = (struct action){-1, EINTR};
        m.actions[2] = (struct action){-1, EAGAIN};
        m.actions[3] = (struct action){(ptrdiff_t)(sizeof(bytes) - prefix), EINTR};
        m.action_count = 4U;
        const struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, bytes, sizeof(bytes), 100U);
        assert(r.status == HS_UART_OK && r.transferred == sizeof(bytes));
        assert(m.calls[POLL] == 1U && memcmp(bytes, m.written, sizeof(bytes)) == 0);
        close_ok(&m, &a, &s); ++cases;
    }
    const int errors[] = {EIO, 0, EAGAIN, EINTR};
    for (unsigned i = 0; i < 4U; ++i) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U); open_ok(&m, &a, &s);
        m.actions[0] = (struct action){9, 0}; m.actions[1] = (struct action){-1, errors[i]};
        m.action_count = 2U; m.poll_rc = 0; m.revents = 0U;
        const struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, bytes, sizeof(bytes), 41U);
        assert(r.status == (i == 2U ? HS_UART_TIMEOUT : i == 3U ? HS_UART_BUDGET : HS_UART_IO_ERROR));
        assert(r.transferred == 9U && m.written_count == 9U && m.calls[CLOSE] == 1U);
        ++cases;
    }
}
static void test_poll_cancel_deadlines(void)
{
    const uint16_t events[] = {1U, 4U, 8U, 16U, 32U, 9U, 17U, 33U};
    const unsigned mapped[] = {HS_UART_READABLE, HS_UART_WRITABLE, HS_UART_ERROR, HS_UART_HUP,
        HS_UART_INVALID_FD, HS_UART_READABLE | HS_UART_ERROR, HS_UART_READABLE | HS_UART_HUP,
        HS_UART_READABLE | HS_UART_INVALID_FD};
    for (unsigned i = 0; i < 8U; ++i) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U); open_ok(&m, &a, &s); m.revents = events[i];
        enum hs_aml_uart_io_error e; unsigned actual = 0U;
        assert(ops->wait(&a, m.fd, HS_UART_READABLE, 1U, &actual, &e) == 1);
        assert(e == HS_UART_IO_NONE && actual == mapped[i]); close_ok(&m, &a, &s); ++cases;
    }
    for (unsigned mode = 0; mode < 6U; ++mode) {
        struct mock m; struct hs_aml_uart_linux a; uint8_t b = 0U;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U); open_ok(&m, &a, &s);
        m.action_count = 1U; m.actions[0] = (struct action){mode == 1U ? 0 : -1, EAGAIN};
        m.revents = 0U; m.poll_rc = 0;
        if (mode == 1U) m.polls_to_cancel = 1U;
        if (mode == 2U) m.frozen = true;
        if (mode == 3U) { m.poll_rc = 1; m.revents = 16U; }
        if (mode == 4U) inject(&m, POLL, -1, EINTR, 1U, 0U);
        if (mode == 5U) inject(&m, POLL, -1, EAGAIN, 1U, 0U);
        const struct hs_aml_uart_result r = hs_aml_uart_read_some(&s, &b, 1U, 41U);
        assert(r.status == (mode == 1U ? HS_UART_CANCELLED : mode == 2U ? HS_UART_BUDGET :
                            mode == 3U ? HS_UART_HANGUP : HS_UART_TIMEOUT));
        assert(r.transferred == 0U && r.cleanup_errors == 0U && m.calls[CLOSE] == 1U);
        assert(m.max_poll <= 20U);
        if (mode == 0U) assert(m.now == 41U && m.calls[POLL] == 3U);
        if (mode == 1U) assert(m.now == 20U && m.calls[POLL] == 1U);
        if (mode == 2U) assert(m.calls[POLL] < HS_AML_UART_MAX_ATTEMPTS);
        ++cases;
    }
}
static void test_invalid_syscalls_and_clock(void)
{
    for (unsigned i = 0; i < 8U; ++i) {
        struct mock m; struct hs_aml_uart_linux a; uint8_t b = 0U;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U); open_ok(&m, &a, &s);
        enum hs_aml_uart_io_error e = HS_UART_IO_NONE; unsigned ev = 0U;
        switch (i) {
        case 0: inject(&m, READ, -2, EINTR, 1U, 0U); assert(ops->read(&a, m.fd, &b, 1U, &e) == -1); break;
        case 1: inject(&m, WRITE, 2, 0, 1U, 0U); assert(ops->write(&a, m.fd, &b, 1U, &e) == -1); break;
        case 2: m.poll_rc = 2; assert(ops->wait(&a, m.fd, HS_UART_READABLE, 1U, &ev, &e) == -1); break;
        case 3: m.poll_rc = 0; assert(ops->wait(&a, m.fd, HS_UART_READABLE, 1U, &ev, &e) == -1); break;
        case 4: m.revents = 0U; assert(ops->wait(&a, m.fd, HS_UART_READABLE, 1U, &ev, &e) == -1); break;
        case 5: m.revents = 2U; assert(ops->wait(&a, m.fd, HS_UART_READABLE, 1U, &ev, &e) == -1); break;
        case 6: inject(&m, STAT, 1, 0, 1U, 0U); assert(ops->read(&a, m.fd, &b, 1U, &e) == -1); break;
        default: inject(&m, GET, 1, 0, 1U, 0U); assert(ops->control(&a, m.fd, HS_AML_UART_TCGETS2, &m.current, &e) == -1); break;
        }
        assert(e == HS_UART_IO_OTHER && a.last_errno == EIO); close_ok(&m, &a, &s); ++cases;
    }
    for (unsigned i = 0; i < 7U; ++i) {
        struct mock m; struct hs_aml_uart_linux a; uint64_t ms = 123U;
        prepare(&m, &a, 0U); m.clock_mode = 1U;
        switch (i) {
        case 0: m.seconds = -1; break; case 1: m.nanoseconds = -1; break;
        case 2: m.nanoseconds = 1000000000; break; case 3: m.seconds = INT64_MAX; break;
        case 4: inject(&m, CLOCK, -1, EINTR, 1U, 0U); break;
        case 5: inject(&m, CLOCK, 1, 0, 1U, 0U); break;
        default: inject(&m, CLOCK, -1, 0, 1U, 0U); break;
        }
        assert(!ops->now_ms(&a, &ms) && ms == 123U);
        assert(a.last_errno == (i < 4U ? EOVERFLOW : i == 4U ? EINTR : EIO)); ++cases;
    }
    struct mock m; struct hs_aml_uart_linux a; uint64_t ms;
    prepare(&m, &a, 0U); m.clock_mode = 1U; m.seconds = 123; m.nanoseconds = 999999999;
    assert(ops->now_ms(&a, &ms) && ms == 123999U);
    assert(!ops->now_ms(&a, NULL));
    m.cancel = true; assert(ops->cancelled(&a));
    assert(hs_aml_uart_linux_init_with_syscalls(&a, 0U, &pins, &sys, &m, NULL, NULL));
    assert(!ops->cancelled(&a)); ++cases;
}
static void test_argument_readiness_config(void)
{
    for (unsigned i = 0; i < 5U; ++i) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        struct hs_aml_uart_readiness r = ready; prepare(&m, &a, 0U);
        switch (i) {
        case 0: r.explicit_uart_opt_in = false; break;
        case 1: r.aml_kernel_binding_verified = false; break;
        case 2: r.character_device_verified = false; break;
        case 3: r.previous_owner_closed = false; break;
        default: m.cancel = true; break;
        }
        assert(hs_aml_uart_open(&s, ops, &a, 0U, r, 100U).status ==
               (i == 4U ? HS_UART_CANCELLED : HS_UART_NOT_READY));
        assert(m.calls[OPEN] == 0U); ++cases;
    }
    for (unsigned i = 0; i < 3U; ++i) {
        struct mock m; struct hs_aml_uart_linux a;
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        prepare(&m, &a, 0U);
        if (i == 0U) m.original.line = m.current.line = 1U;
        if (i == 1U) m.mismatch = true;
        if (i == 2U) inject(&m, GET, -1, EIO, 1U, 1U);
        struct hs_aml_uart_result r = hs_aml_uart_open(&s, ops, &a, 0U, ready, 100U);
        assert(r.status == (i == 0U ? HS_UART_CONFIG_UNSUPPORTED :
                            i == 1U ? HS_UART_CONFIG_MISMATCH : HS_UART_IO_ERROR));
        assert(r.cleanup_errors == 0U && m.calls[CLOSE] == 1U && m.calls[RELEASE] == 1U);
        assert(m.calls[RESTORE] == (i == 0U ? 0U : 1U)); ++cases;
    }
    struct mock m; struct hs_aml_uart_linux a;
    struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
    enum hs_aml_uart_io_error e; unsigned ev; uint8_t b = 0U;
    prepare(&m, &a, 0U);
    assert(ops->open(&a, hs_aml_uart_path(0U), HS_AML_UART_OPEN_FLAGS, &e) == m.fd);
    assert(ops->read(&a, m.fd, &b, 1U, &e) == -1 && e == HS_UART_IO_OTHER);
    assert(ops->control(&a, m.fd, HS_AML_UART_TIOCEXCL, NULL, &e) == -1);
    assert(ops->verify_fd(&a, m.fd, 1U, &e) == -1);
    assert(m.calls[EXCL] == 0U && m.calls[READ] == 0U);
    assert(ops->close(&a, m.fd, &e) == 0); ++cases;
    prepare(&m, &a, 0U); open_ok(&m, &a, &s);
    assert(ops->open(&a, hs_aml_uart_path(0U), HS_AML_UART_OPEN_FLAGS, &e) == -1);
    assert(ops->read(&a, m.fd + 1, &b, 1U, &e) == -1);
    assert(ops->read(&a, m.fd, NULL, 1U, &e) == -1);
    assert(ops->read(&a, m.fd, &b, 0U, &e) == -1);
    assert(ops->write(&a, m.fd, &b, HS_AML_UART_MAX_BYTES + 1U, &e) == -1);
    assert(ops->control(&a, m.fd, UINT32_C(0x540b), NULL, &e) == -1);
    assert(ops->control(&a, m.fd, HS_AML_UART_TCSETS2, NULL, &e) == -1);
    assert(ops->control(&a, m.fd, HS_AML_UART_TIOCEXCL, &b, &e) == -1);
    assert(ops->wait(&a, m.fd, 3U, 1U, &ev, &e) == -1);
    assert(ops->wait(&a, m.fd, HS_UART_READABLE, 0U, &ev, &e) == -1);
    assert(ops->wait(&a, m.fd, HS_UART_READABLE, 21U, &ev, &e) == -1);
    assert(ops->wait(&a, m.fd, HS_UART_READABLE, 1U, NULL, &e) == -1);
    assert(ops->close(&a, m.fd + 1, &e) == -1 && a.fd == m.fd);
    struct hs_aml_uart_linux copied = a;
    assert(ops->write(&copied, m.fd, &b, 1U, &e) == -1);
    assert(ops->close(&copied, m.fd, &e) == -1 && a.fd == m.fd);
    assert(m.calls[POLL] == 0U && m.calls[READ] == 0U && m.calls[WRITE] == 0U);
    close_ok(&m, &a, &s); ++cases;
    prepare(&m, &a, 0U); struct hs_aml_uart_readiness read_only = ready;
    read_only.byte_writes_authorized = false;
    assert(hs_aml_uart_open(&s, ops, &a, 0U, read_only, 100U).status == HS_UART_OK);
    assert(hs_aml_uart_write_all(&s, &b, 1U, 100U).status == HS_UART_NOT_READY);
    assert(s.active && m.calls[WRITE] == 0U); close_ok(&m, &a, &s); ++cases;
    prepare(&m, &a, 0U); open_ok(&m, &a, &s);
    inject(&m, STAT, -1, EINTR, 4U, 2U);
    assert(hs_aml_uart_close(&s) == HS_UART_CLOSE_FAILED);
    assert(a.close_uncertain && a.close_errno == EINTR && a.fd == -1 && m.calls[CLOSE] == 0U);
    ++cases;
}
int main(void)
{
    ops = hs_aml_uart_linux_ops();
    test_mapping_lifecycle(); test_init_guards(); test_bad_pins_flags(); test_open_faults();
    test_cleanup_and_identity(); test_partial_and_errors(); test_poll_cancel_deadlines();
    test_invalid_syscalls_and_clock(); test_argument_readiness_config();
    printf("aml_uart_linux: %u deterministic cases passed (injected syscalls only)\n", cases);
    return 0;
}
