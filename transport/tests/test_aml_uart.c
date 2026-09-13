/* SPDX-License-Identifier: GPL-3.0-only */


#include "hs_aml_uart.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum stage {OPEN = 1, VERIFY, EXCLUSIVE, GET, SET, READBACK, RESTORE, CLOSE, RELEASE};
struct io_action { ptrdiff_t count; enum hs_aml_uart_io_error error; };
struct wait_action { int rc; unsigned events; enum hs_aml_uart_io_error error; };
struct mock {
    struct hs_aml_uart_termios2 original, current;
    uint64_t now, step;
    unsigned clocks, fail_clock_at, backward_at, cancel_after, callbacks;
    unsigned clock_fault_after_callback, clock_fault_kind;
    unsigned fail_stage, interrupt_stage, interrupts_left;
    unsigned open_calls, close_calls, gets, sets, restores, releases, read_calls, write_calls, waits;
    unsigned chain, io_index, wait_index, action_count, wait_count;
    bool mismatch, restore_fail, release_fail, close_fail, freeze_wait, forced_cancel;
    bool negative_two, verify_positive;
    struct io_action actions[16];
    struct wait_action wait_actions[16];
    uint8_t written[HS_AML_UART_MAX_BYTES], received[64];
    size_t written_count;
};

static unsigned long cases;
static bool stage_fails(struct mock *m, unsigned stage, enum hs_aml_uart_io_error *error)
{
    ++m->callbacks;
    if (m->interrupt_stage == stage && m->interrupts_left != 0U) {
        --m->interrupts_left;
        *error = HS_UART_IO_INTERRUPTED;
        return true;
    }
    if (m->fail_stage == stage) {
        *error = HS_UART_IO_OTHER;
        return true;
    }
    return false;
}
static int mock_open(void *ctx, const char *path, uint32_t flags, enum hs_aml_uart_io_error *error)
{
    struct mock *m = ctx;
    ++m->open_calls;
    assert(strcmp(path, hs_aml_uart_path(m->chain)) == 0);
    assert(flags == UINT32_C(0x88902));
    if (m->negative_two) return -2;
    if (stage_fails(m, OPEN, error)) return -1;
    return 7;
}
static int mock_verify(void *ctx, int fd, unsigned chain, enum hs_aml_uart_io_error *error)
{
    struct mock *m = ctx;
    assert(fd == 7 && chain == m->chain);
    if (m->verify_positive) return 1;
    return stage_fails(m, VERIFY, error) ? -1 : 0;
}
static int mock_control(void *ctx, int fd, uint32_t request, void *arg,
                         enum hs_aml_uart_io_error *error)
{
    struct mock *m = ctx;
    unsigned which;
    assert(fd == 7);
    if (request == HS_AML_UART_TIOCEXCL) {
        assert(arg == NULL);
        return stage_fails(m, EXCLUSIVE, error) ? -1 : 0;
    }
    if (request == HS_AML_UART_TIOCNXCL) {
        assert(arg == NULL);
        ++m->releases;
        if (m->release_fail) { *error = HS_UART_IO_INTERRUPTED; return -1; }
        return stage_fails(m, RELEASE, error) ? -1 : 0;
    }
    assert(arg != NULL);
    if (request == HS_AML_UART_TCGETS2) {
        which = m->sets == 0U ? GET : READBACK;
        ++m->gets;
        if (stage_fails(m, which, error)) return -1;
        *(struct hs_aml_uart_termios2 *)arg = m->current;
        if (m->mismatch && which == READBACK)
            ((struct hs_aml_uart_termios2 *)arg)->ospeed ^= UINT32_C(1);
        return 0;
    }
    assert(request == HS_AML_UART_TCSETS2);
    which = memcmp(arg, &m->original, sizeof(m->original)) == 0 ? RESTORE : SET;
    if (which == RESTORE) {
        ++m->restores;
        if (m->restore_fail) { *error = HS_UART_IO_INTERRUPTED; return -1; }
    } else {
        ++m->sets;
    }

    m->current = *(struct hs_aml_uart_termios2 *)arg;
    return stage_fails(m, which, error) ? -1 : 0;
}
static ptrdiff_t mock_io(struct mock *m, size_t length, enum hs_aml_uart_io_error *error)
{
    ++m->callbacks;
    if (m->action_count != 0U) {
        unsigned index = m->io_index < m->action_count ? m->io_index : m->action_count - 1U;
        ++m->io_index;
        *error = m->actions[index].error;
        return m->actions[index].count;
    }
    return (ptrdiff_t)length;
}
static ptrdiff_t mock_read(void *ctx, int fd, uint8_t *buffer, size_t length,
                            enum hs_aml_uart_io_error *error)
{
    struct mock *m = ctx;
    ptrdiff_t count;
    size_t i;
    assert(fd == 7);
    ++m->read_calls;
    count = mock_io(m, length, error);
    if (count > 0 && (size_t)count <= length) {
        for (i = 0U; i < (size_t)count; ++i) buffer[i] = (uint8_t)(i & 255U);
    }
    return count;
}
static ptrdiff_t mock_write(void *ctx, int fd, const uint8_t *buffer, size_t length,
                             enum hs_aml_uart_io_error *error)
{
    struct mock *m = ctx;
    ptrdiff_t count;
    assert(fd == 7);
    ++m->write_calls;
    count = mock_io(m, length, error);
    if (count > 0 && (size_t)count <= length) {
        assert(m->written_count + (size_t)count <= sizeof(m->written));
        memcpy(m->written + m->written_count, buffer, (size_t)count);
        m->written_count += (size_t)count;
    }
    return count;
}
static int mock_wait(void *ctx, int fd, unsigned requested, unsigned timeout_ms,
                      unsigned *events, enum hs_aml_uart_io_error *error)
{
    struct mock *m = ctx;
    unsigned index;
    assert(fd == 7 && (requested == HS_UART_READABLE || requested == HS_UART_WRITABLE));
    assert(timeout_ms > 0U && timeout_ms <= HS_AML_UART_POLL_SLICE_MS);
    ++m->callbacks;
    ++m->waits;
    if (!m->freeze_wait) m->now += timeout_ms;
    if (m->wait_count != 0U) {
        index = m->wait_index < m->wait_count ? m->wait_index : m->wait_count - 1U;
        ++m->wait_index;
        *events = m->wait_actions[index].events;
        *error = m->wait_actions[index].error;
        return m->wait_actions[index].rc;
    }
    *events = requested;
    return 1;
}
static int mock_close(void *ctx, int fd, enum hs_aml_uart_io_error *error)
{
    struct mock *m = ctx;
    assert(fd == 7);
    ++m->close_calls;
    if (m->close_fail) { *error = HS_UART_IO_INTERRUPTED; return -1; }
    return stage_fails(m, CLOSE, error) ? -1 : 0;
}
static bool mock_now(void *ctx, uint64_t *value)
{
    struct mock *m = ctx;
    ++m->clocks;
    if (m->fail_clock_at == m->clocks) return false;
    if (m->clock_fault_after_callback != 0U && m->callbacks >= m->clock_fault_after_callback) {
        if (m->clock_fault_kind == 1U) return false;
        if (m->clock_fault_kind == 2U) { *value = 0U; return true; }
        if (m->clock_fault_kind == 3U) { m->now += 100000U; *value = m->now; return true; }
    }
    m->now += m->step;
    if (m->backward_at == m->clocks) m->now = 0U;
    *value = m->now;
    return true;
}
static bool mock_cancel(void *ctx)
{
    struct mock *m = ctx;
    return m->forced_cancel || (m->cancel_after != 0U && m->callbacks >= m->cancel_after);
}

static const struct hs_aml_uart_ops ops = {
    mock_open, mock_verify, mock_control, mock_read, mock_write, mock_wait,
    mock_close, mock_now, mock_cancel
};
static struct hs_aml_uart_readiness ready(void)
{
    struct hs_aml_uart_readiness r = {true, true, true, true, true};
    return r;
}
static void fixture(struct mock *m)
{
    size_t i;
    memset(m, 0, sizeof(*m));
    m->now = 1000U;
    m->original.iflag = UINT32_C(0x1020);
    m->original.oflag = 1U;
    m->original.cflag = UINT32_C(0x400);
    m->original.lflag = UINT32_C(0x804b);
    for (i = 0U; i < sizeof(m->original.cc); ++i) m->original.cc[i] = (uint8_t)i;
    m->original.ispeed = UINT32_C(9600);
    m->original.ospeed = UINT32_C(9600);
    m->current = m->original;
}
static void opened(struct mock *m, struct hs_aml_uart_session *s)
{
    struct hs_aml_uart_result r = hs_aml_uart_open(s, &ops, m, m->chain, ready(), 1000U);
    assert(r.status == HS_UART_OK && r.cleanup_errors == 0U);
    assert(s->active && s->fd == 7 && m->sets == 1U && m->gets == 2U);
}
static void closed(struct mock *m, struct hs_aml_uart_session *s)
{
    assert(hs_aml_uart_close(s) == 0U);
    assert(!s->active && s->fd == -1 && m->close_calls == 1U);
    assert(m->restores == 1U && memcmp(&m->current, &m->original, sizeof(m->current)) == 0);
    assert(hs_aml_uart_close(s) == 0U && m->close_calls == 1U);
}

static void test_profile(void)
{
    uint32_t seed = UINT32_C(0x917218);
    unsigned i, j;
    struct hs_aml_uart_termios2 original, configured, expected;
    for (i = 0U; i < 10000U; ++i) {
        uint8_t *bytes = (uint8_t *)&original;
        for (j = 0U; j < sizeof(original); ++j) {
            seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[j] = (uint8_t)(seed >> 24);
        }
        expected = original;
        expected.iflag &= ~UINT32_C(0x5eb);
        expected.oflag &= ~UINT32_C(1);
        expected.lflag &= ~UINT32_C(0x804b);
        expected.cflag = (expected.cflag & ~UINT32_C(0x9b0)) | UINT32_C(0x8b0);
        expected.cflag = (expected.cflag & ~UINT32_C(0x100f100f)) | UINT32_C(0x10001000);
        expected.cc[5] = 0U; expected.cc[6] = 7U;
        expected.ispeed = UINT32_C(115200); expected.ospeed = UINT32_C(115200);
        assert(hs_aml_uart_make_profile(&original, &configured));
        assert(memcmp(&configured, &expected, sizeof(expected)) == 0);
        configured = original;
        assert(hs_aml_uart_make_profile(&configured, &configured));
        assert(memcmp(&configured, &expected, sizeof(expected)) == 0);
        ++cases;
    }
    memset(&configured, 0xa5, sizeof(configured)); expected = configured;
    assert(!hs_aml_uart_make_profile(NULL, &configured));
    assert(memcmp(&configured, &expected, sizeof(expected)) == 0);
    assert(!hs_aml_uart_make_profile(&original, NULL));
    assert(!hs_aml_uart_make_profile((const struct hs_aml_uart_termios2 *)UINTPTR_MAX, &configured));
    assert(hs_aml_uart_path(3U) == NULL && hs_aml_uart_path(UINT_MAX) == NULL);
    cases += 4UL;
}

static void test_open(void)
{
    struct mock m;
    unsigned chain, stage, flag;
    for (chain = 0U; chain < 3U; ++chain) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); m.chain = chain;
        opened(&m, &s); closed(&m, &s); ++cases;
    }
    for (flag = 0U; flag < 4U; ++flag) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        struct hs_aml_uart_readiness r = ready();
        fixture(&m);
        if (flag == 0U) r.explicit_uart_opt_in = false;
        if (flag == 1U) r.aml_kernel_binding_verified = false;
        if (flag == 2U) r.character_device_verified = false;
        if (flag == 3U) r.previous_owner_closed = false;
        assert(hs_aml_uart_open(&s, &ops, &m, 0U, r, 100U).status == HS_UART_NOT_READY);
        assert(m.open_calls == 0U); ++cases;
    }
    for (stage = OPEN; stage <= READBACK; ++stage) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        struct hs_aml_uart_result r;
        fixture(&m); m.fail_stage = stage;
        r = hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 100U);
        assert(r.status == (stage == VERIFY ? HS_UART_NOT_READY : HS_UART_IO_ERROR));
        assert(!s.active && s.fd == -1);
        assert(m.close_calls == (stage == OPEN ? 0U : 1U));
        assert(m.restores == (stage >= SET ? 1U : 0U));
        assert(r.cleanup_errors == 0U); ++cases;
    }
    for (stage = OPEN; stage <= READBACK; ++stage) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); m.interrupt_stage = stage; m.interrupts_left = 3U;
        assert(hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 100U).status == HS_UART_OK);
        assert(m.interrupts_left == 0U);
        closed(&m, &s); ++cases;
    }
    for (flag = 0U; flag < 4U; ++flag) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        struct hs_aml_uart_result r;
        fixture(&m);
        m.mismatch = flag == 0U;
        m.negative_two = flag == 1U;
        m.verify_positive = flag == 2U;
        if (flag == 3U) { m.original.line = 1U; m.current = m.original; }
        r = hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 100U);
        assert(r.status == (flag == 0U ? HS_UART_CONFIG_MISMATCH :
                            flag == 3U ? HS_UART_CONFIG_UNSUPPORTED : HS_UART_CALLBACK_ERROR));
        assert(m.close_calls == (flag == 1U ? 0U : 1U)); ++cases;
    }
    for (flag = 0U; flag < 7U; ++flag) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        struct hs_aml_uart_ops broken = ops;
        struct hs_aml_uart_result r;
        fixture(&m);
        if (flag == 0U) m.forced_cancel = true;
        if (flag == 1U) m.fail_clock_at = 1U;
        if (flag == 2U) m.now = UINT64_MAX - 1U;
        if (flag == 3U) m.step = 100U;
        if (flag == 4U) m.backward_at = 2U;
        if (flag == 5U) { m.interrupt_stage = OPEN; m.interrupts_left = UINT_MAX; }
        if (flag == 6U) broken.control = NULL;
        r = hs_aml_uart_open(&s, &broken, &m, 0U, ready(), 100U);
        assert(r.status == (flag == 0U ? HS_UART_CANCELLED :
            flag == 3U ? HS_UART_TIMEOUT : flag == 5U ? HS_UART_BUDGET :
            flag == 6U ? HS_UART_ARGUMENT : HS_UART_CLOCK_ERROR));
        assert(m.close_calls == 0U && !s.active); ++cases;
    }
    {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); m.fail_stage = SET; m.restore_fail = true; m.release_fail = true; m.close_fail = true;
        struct hs_aml_uart_result r = hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 100U);
        assert(r.status == HS_UART_IO_ERROR && r.cleanup_errors == 7U);
        assert(m.restores == 4U && m.releases == 4U && m.close_calls == 1U && !s.active); ++cases;
    }
}

static void test_transfers(void)
{
    struct mock m;
    uint8_t source[256], destination[32];
    unsigned i, flag;
    for (i = 0U; i < sizeof(source); ++i) source[i] = (uint8_t)i;
    for (i = 1U; i <= sizeof(source); ++i) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s);
        m.actions[0] = (struct io_action){1, HS_UART_IO_NONE}; m.action_count = 1U;
        struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, source, i, 1000U);
        assert(r.status == HS_UART_OK && r.transferred == i && m.write_calls == i);
        assert(m.written_count == i && memcmp(m.written, source, i) == 0);
        closed(&m, &s); ++cases;
    }
    {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s);
        m.actions[0] = (struct io_action){3, HS_UART_IO_NONE};
        m.actions[1] = (struct io_action){-1, HS_UART_IO_INTERRUPTED};
        m.actions[2] = (struct io_action){-1, HS_UART_IO_AGAIN};
        m.actions[3] = (struct io_action){0, HS_UART_IO_NONE};
        m.actions[4] = (struct io_action){5, HS_UART_IO_NONE}; m.action_count = 5U;
        m.wait_actions[0] = (struct wait_action){-1, 0U, HS_UART_IO_INTERRUPTED};
        m.wait_actions[1] = (struct wait_action){-1, 0U, HS_UART_IO_AGAIN};
        m.wait_actions[2] = (struct wait_action){1, HS_UART_WRITABLE, HS_UART_IO_NONE}; m.wait_count = 3U;
        struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, source, 8U, 1000U);
        assert(r.status == HS_UART_OK && r.transferred == 8U && m.waits == 4U);
        assert(memcmp(m.written, source, 8U) == 0); closed(&m, &s); ++cases;
    }
    for (flag = 0U; flag < 11U; ++flag) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s);
        m.actions[0] = (struct io_action){3, HS_UART_IO_NONE};
        m.actions[1] = (struct io_action){-1, HS_UART_IO_AGAIN}; m.action_count = 2U;
        enum hs_aml_uart_status expected = HS_UART_IO_ERROR;
        if (flag == 0U) { m.actions[1].error = HS_UART_IO_OTHER; }
        if (flag == 1U) { m.actions[1].count = 99; expected = HS_UART_CALLBACK_ERROR; }
        if (flag == 2U) { m.actions[1].count = -2; expected = HS_UART_CALLBACK_ERROR; }
        if (flag == 3U) { m.wait_actions[0] = (struct wait_action){1,HS_UART_HUP,HS_UART_IO_NONE}; m.wait_count = 1U; expected = HS_UART_HANGUP; }
        if (flag == 4U) { m.wait_actions[0] = (struct wait_action){1,HS_UART_ERROR,HS_UART_IO_NONE}; m.wait_count = 1U; }
        if (flag == 5U) { m.wait_actions[0] = (struct wait_action){1,HS_UART_INVALID_FD,HS_UART_IO_NONE}; m.wait_count = 1U; }
        if (flag == 6U) { m.wait_actions[0] = (struct wait_action){0,HS_UART_WRITABLE,HS_UART_IO_NONE}; m.wait_count = 1U; expected = HS_UART_CALLBACK_ERROR; }
        if (flag == 7U) { m.wait_actions[0] = (struct wait_action){1,32U,HS_UART_IO_NONE}; m.wait_count = 1U; expected = HS_UART_CALLBACK_ERROR; }
        if (flag == 8U) { m.wait_actions[0] = (struct wait_action){2,0U,HS_UART_IO_NONE}; m.wait_count = 1U; expected = HS_UART_CALLBACK_ERROR; }
        if (flag == 9U) { expected = HS_UART_TIMEOUT; }
        if (flag == 10U) { m.freeze_wait = true; expected = HS_UART_BUDGET; }
        struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, source, 8U, 100U);
        assert(r.status == expected && r.transferred == 3U && m.written_count == 3U);
        assert(m.close_calls == 1U && m.restores == 1U && !s.active); ++cases;
    }
    for (flag = 0U; flag < 5U; ++flag) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s);
        m.actions[0] = (struct io_action){3, HS_UART_IO_NONE};
        m.actions[1] = (struct io_action){-1, HS_UART_IO_AGAIN}; m.action_count = 2U;
        if (flag == 0U) m.cancel_after = m.callbacks + 1U;
        if (flag == 1U) m.fail_clock_at = m.clocks + 3U;
        if (flag == 2U) m.backward_at = m.clocks + 3U;
        if (flag == 3U) { m.cancel_after = m.callbacks + 3U; m.restore_fail = true; m.close_fail = true; }
        if (flag == 4U) { m.interrupt_stage = RESTORE; m.interrupts_left = 3U; m.cancel_after = m.callbacks + 1U; }
        struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, source, 8U, 100U);
        assert(r.status == ((flag == 1U || flag == 2U) ? HS_UART_CLOCK_ERROR : HS_UART_CANCELLED));
        assert(r.transferred == 3U && m.close_calls == 1U);
        assert(r.cleanup_errors == (flag == 3U ? 3U : 0U)); ++cases;
    }
    {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s);
        m.actions[0] = (struct io_action){-1, HS_UART_IO_INTERRUPTED};
        m.actions[1] = (struct io_action){0, HS_UART_IO_NONE};
        m.actions[2] = (struct io_action){-1, HS_UART_IO_AGAIN};
        m.actions[3] = (struct io_action){7, HS_UART_IO_NONE}; m.action_count = 4U;
        memset(destination, 0xee, sizeof(destination));
        struct hs_aml_uart_result r = hs_aml_uart_read_some(&s, destination, sizeof(destination), 100U);
        assert(r.status == HS_UART_OK && r.transferred == 7U && m.read_calls == 4U);
        for (i = 0U; i < 7U; ++i) assert(destination[i] == (uint8_t)i);
        for (i = 7U; i < sizeof(destination); ++i) assert(destination[i] == UINT8_C(0xee));
        closed(&m, &s); ++cases;
    }
    {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s); s.writes_authorized = false;
        assert(hs_aml_uart_write_all(&s, source, 8U, 100U).status == HS_UART_NOT_READY);
        assert(hs_aml_uart_read_some(&s, destination, sizeof(destination), 0U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_read_some(&s, destination, 0U, 100U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_read_some(&s, destination, HS_AML_UART_MAX_BYTES + 1U, 100U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_read_some(&s, NULL, 1U, 100U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_read_some(&s, (uint8_t *)UINTPTR_MAX, 2U, 100U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 100U).status == HS_UART_ARGUMENT);
        assert(m.read_calls == 0U && m.write_calls == 0U && s.active);
        closed(&m, &s);
        assert(hs_aml_uart_read_some(&s, destination, 1U, 100U).status == HS_UART_NOT_OPEN);
        cases += 8UL;
    }
}

static void test_boundaries(void)
{
    struct mock m;
    uint8_t byte = 1U;
    unsigned i, fault, writing;
    for (i = 1U; i <= 6U; ++i) {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); m.cancel_after = i;
        struct hs_aml_uart_result r = hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 100U);
        assert(r.status == HS_UART_CANCELLED && !s.active && s.fd == -1);
        assert(m.close_calls == 1U);
        assert(m.restores == (i >= 5U ? 1U : 0U)); ++cases;
    }
    for (i = 1U; i <= 6U; ++i) {
        for (fault = 1U; fault <= 3U; ++fault) {
            struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
            fixture(&m); m.clock_fault_after_callback = i; m.clock_fault_kind = fault;
            struct hs_aml_uart_result r = hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 100U);
            assert(r.status == (fault == 3U ? HS_UART_TIMEOUT : HS_UART_CLOCK_ERROR));
            assert(!s.active && s.fd == -1 && m.close_calls == 1U);
            assert(m.restores == (i >= 5U ? 1U : 0U));
            assert(m.releases == (i >= 3U ? 1U : 0U)); ++cases;
        }
    }
    for (writing = 0U; writing < 2U; ++writing) {
        for (fault = 0U; fault <= 3U; ++fault) {
            struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
            fixture(&m); opened(&m, &s);
            if (fault == 0U) m.cancel_after = m.callbacks + 1U;
            else { m.clock_fault_after_callback = m.callbacks + 1U; m.clock_fault_kind = fault; }
            struct hs_aml_uart_result r = writing != 0U ?
                hs_aml_uart_write_all(&s, &byte, 1U, 100U) : hs_aml_uart_read_some(&s, &byte, 1U, 100U);
            assert(r.status == (fault == 0U ? HS_UART_CANCELLED : fault == 3U ? HS_UART_TIMEOUT : HS_UART_CLOCK_ERROR));
            assert(r.transferred == 1U && m.close_calls == 1U && m.restores == 1U && m.releases == 1U);
            assert(m.write_calls + m.read_calls == 1U); ++cases;
        }
    }
    {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m);
        assert(hs_aml_uart_open(&s, &ops, &m, 3U, ready(), 100U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_open(&s, &ops, &m, UINT_MAX, ready(), 100U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_open(&s, &ops, &m, 0U, ready(), 0U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_open(&s, &ops, &m, 0U, ready(), HS_AML_UART_MAX_TIMEOUT_MS + 1U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_open(&s, NULL, &m, 0U, ready(), 100U).status == HS_UART_ARGUMENT);
        assert(hs_aml_uart_open(NULL, &ops, &m, 0U, ready(), 100U).status == HS_UART_ARGUMENT);
        assert(m.open_calls == 0U);
        opened(&m, &s);
        assert(hs_aml_uart_write_all(&s, &byte, 1U, UINT32_MAX).status == HS_UART_ARGUMENT);
        assert(s.active && m.write_calls == 0U);
        m.forced_cancel = true;
        struct hs_aml_uart_result r = hs_aml_uart_read_some(&s, &byte, 1U, 100U);
        assert(r.status == HS_UART_CANCELLED && m.read_calls == 0U && m.close_calls == 1U);
        cases += 8UL;
    }
    {
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s); m.actions[0] = (struct io_action){1, HS_UART_IO_NONE}; m.action_count = 1U;
        m.cancel_after = m.callbacks + 1U;
        struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, &byte, 1U, 100U);

        assert(r.status == HS_UART_CANCELLED && r.transferred == 1U && m.written_count == 1U);
        assert(m.close_calls == 1U); ++cases;
    }
    {
        static const uint8_t maximum[HS_AML_UART_MAX_BYTES] = {1U};
        struct hs_aml_uart_session s = HS_AML_UART_SESSION_INIT;
        fixture(&m); opened(&m, &s);
        struct hs_aml_uart_result r = hs_aml_uart_write_all(&s, maximum, sizeof(maximum), 100U);
        assert(r.status == HS_UART_OK && r.transferred == sizeof(maximum));
        assert(memcmp(m.written, maximum, sizeof(maximum)) == 0);
        closed(&m, &s); ++cases;
    }
}

int main(void)
{
    test_profile(); test_open(); test_transfers(); test_boundaries();
    printf("AML UART injected transport: %lu cases passed; no real files/devices opened\n", cases);
    return 0;
}
