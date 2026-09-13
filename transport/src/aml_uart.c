/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_uart.h"
#include "hs_span.h"
#include <string.h>

_Static_assert(sizeof(struct hs_aml_uart_termios2) == 44U, "ARM termios2 size");
_Static_assert(offsetof(struct hs_aml_uart_termios2, cc) == 17U, "ARM termios2 cc");
_Static_assert(offsetof(struct hs_aml_uart_termios2, ispeed) == 36U, "ARM input baud");
_Static_assert(offsetof(struct hs_aml_uart_termios2, ospeed) == 40U, "ARM output baud");

struct deadline {
    uint64_t last, end;
    unsigned attempts;
};

static struct hs_aml_uart_result result(enum hs_aml_uart_status status,
                                         size_t bytes, unsigned cleanup)
{
    struct hs_aml_uart_result out = {status, bytes, cleanup};
    return out;
}

const char *hs_aml_uart_path(unsigned chain)
{
    static const char *const paths[3] = {"/dev/ttyS3", "/dev/ttyS2", "/dev/ttyS1"};
    return chain < 3U ? paths[chain] : NULL;
}

bool hs_aml_uart_make_profile(const struct hs_aml_uart_termios2 *original,
                              struct hs_aml_uart_termios2 *configured)
{
    struct hs_aml_uart_termios2 value;
    if (!hs_span_nonempty(original, sizeof(*original)) || !hs_span_nonempty(configured, sizeof(*configured))) return false;
    value = *original;
    value.iflag &= UINT32_C(0xfffffa14);
    value.oflag &= UINT32_C(0xfffffffe);
    value.lflag &= UINT32_C(0xffff7fb4);
    value.cflag = (value.cflag & UINT32_C(0xfffff64f)) | UINT32_C(0x8b0);
    value.cc[5] = 0U;
    value.cc[6] = 7U;
    value.cflag = (value.cflag & UINT32_C(0xeff0eff0)) | UINT32_C(0x10001000);
    value.ispeed = UINT32_C(115200);
    value.ospeed = UINT32_C(115200);
    *configured = value;
    return true;
}

static bool ops_valid(const struct hs_aml_uart_ops *ops)
{
    return ops != NULL && ops->open != NULL && ops->verify_fd != NULL &&
           ops->control != NULL && ops->read != NULL && ops->write != NULL &&
           ops->wait != NULL && ops->close != NULL && ops->now_ms != NULL &&
           ops->cancelled != NULL;
}

static enum hs_aml_uart_status begin(struct hs_aml_uart_session *s,
                                     uint32_t timeout_ms, struct deadline *d)
{
    uint64_t now;
    if (timeout_ms == 0U || timeout_ms > HS_AML_UART_MAX_TIMEOUT_MS) return HS_UART_ARGUMENT;
    if (s->ops.cancelled(s->context)) return HS_UART_CANCELLED;
    if (!s->ops.now_ms(s->context, &now) || UINT64_MAX - now < timeout_ms) return HS_UART_CLOCK_ERROR;
    d->last = now;
    d->end = now + timeout_ms;
    d->attempts = 0U;
    return HS_UART_OK;
}

static enum hs_aml_uart_status guard(struct hs_aml_uart_session *s, struct deadline *d)
{
    uint64_t now;
    if (s->ops.cancelled(s->context)) return HS_UART_CANCELLED;
    if (!s->ops.now_ms(s->context, &now) || now < d->last) return HS_UART_CLOCK_ERROR;
    d->last = now;
    if (now >= d->end) return HS_UART_TIMEOUT;
    if (d->attempts >= HS_AML_UART_MAX_ATTEMPTS) return HS_UART_BUDGET;
    ++d->attempts;
    return HS_UART_OK;
}

unsigned hs_aml_uart_close(struct hs_aml_uart_session *s)
{
    unsigned errors = 0U, retry;
    int fd, rc = -1;
    enum hs_aml_uart_io_error error;
    if (s == NULL || !s->active) return 0U;
    fd = s->fd;
    if (s->restore_needed && s->original_valid) {
        for (retry = 0U; retry < 4U; ++retry) {
            error = HS_UART_IO_NONE;
            rc = s->ops.control(s->context, fd, HS_AML_UART_TCSETS2, &s->original, &error);
            if (rc == 0 || rc != -1 || error != HS_UART_IO_INTERRUPTED) break;
        }
        if (rc != 0) errors |= HS_UART_RESTORE_FAILED;
    }
    if (s->exclusive_attempted) {
        for (retry = 0U; retry < 4U; ++retry) {
            error = HS_UART_IO_NONE;
            rc = s->ops.control(s->context, fd, HS_AML_UART_TIOCNXCL, NULL, &error);
            if (rc == 0 || rc != -1 || error != HS_UART_IO_INTERRUPTED) break;
        }
        if (rc != 0) errors |= HS_UART_EXCLUSIVE_RELEASE_FAILED;
    }
    s->fd = -1;
    s->active = false;
    s->restore_needed = false;
    s->original_valid = false;
    s->exclusive_attempted = false;
    s->writes_authorized = false;
    error = HS_UART_IO_NONE;

    if (s->ops.close(s->context, fd, &error) != 0) errors |= HS_UART_CLOSE_FAILED;
    return errors;
}

static struct hs_aml_uart_result fail(struct hs_aml_uart_session *s,
                                      enum hs_aml_uart_status status, size_t bytes)
{
    return result(status, bytes, hs_aml_uart_close(s));
}

static enum hs_aml_uart_status control(struct hs_aml_uart_session *s,
                                       struct deadline *d, uint32_t request, void *arg)
{
    int rc;
    enum hs_aml_uart_status status;
    enum hs_aml_uart_io_error error;
    for (;;) {
        status = guard(s, d);
        if (status != HS_UART_OK) return status;
        error = HS_UART_IO_NONE;
        if (request == HS_AML_UART_TCSETS2) s->restore_needed = true;
        if (request == HS_AML_UART_TIOCEXCL) s->exclusive_attempted = true;
        rc = s->ops.control(s->context, s->fd, request, arg, &error);
        if (rc == 0) return HS_UART_OK;
        if (rc != -1) return HS_UART_CALLBACK_ERROR;
        if (error != HS_UART_IO_INTERRUPTED) return HS_UART_IO_ERROR;
    }
}

struct hs_aml_uart_result hs_aml_uart_open(
    struct hs_aml_uart_session *s, const struct hs_aml_uart_ops *ops, void *context,
    unsigned chain, struct hs_aml_uart_readiness readiness, uint32_t timeout_ms)
{
    struct hs_aml_uart_session next = HS_AML_UART_SESSION_INIT;
    struct hs_aml_uart_termios2 config, observed;
    struct deadline d;
    enum hs_aml_uart_status status;
    enum hs_aml_uart_io_error error;
    int fd, rc;
    if (s == NULL || s->active || s->fd != -1 || !ops_valid(ops) ||
        hs_aml_uart_path(chain) == NULL || timeout_ms == 0U ||
        timeout_ms > HS_AML_UART_MAX_TIMEOUT_MS) return result(HS_UART_ARGUMENT, 0U, 0U);
    if (!readiness.explicit_uart_opt_in || !readiness.aml_kernel_binding_verified ||
        !readiness.character_device_verified || !readiness.previous_owner_closed)
        return result(HS_UART_NOT_READY, 0U, 0U);
    next.ops = *ops;
    next.context = context;
    next.chain = chain;
    next.writes_authorized = readiness.byte_writes_authorized;
    status = begin(&next, timeout_ms, &d);
    if (status != HS_UART_OK) return result(status, 0U, 0U);
    for (;;) {
        status = guard(&next, &d);
        if (status != HS_UART_OK) return result(status, 0U, 0U);
        error = HS_UART_IO_NONE;
        fd = next.ops.open(context, hs_aml_uart_path(chain), HS_AML_UART_OPEN_FLAGS, &error);
        if (fd >= 0) break;
        if (fd != -1) return result(HS_UART_CALLBACK_ERROR, 0U, 0U);
        if (error != HS_UART_IO_INTERRUPTED) return result(HS_UART_IO_ERROR, 0U, 0U);
    }
    next.fd = fd;
    next.active = true;
    for (;;) {
        status = guard(&next, &d);
        if (status != HS_UART_OK) return fail(&next, status, 0U);
        error = HS_UART_IO_NONE;
        rc = next.ops.verify_fd(context, fd, chain, &error);
        if (rc == 0) break;
        if (rc != -1) return fail(&next, HS_UART_CALLBACK_ERROR, 0U);
        if (error != HS_UART_IO_INTERRUPTED) return fail(&next, HS_UART_NOT_READY, 0U);
    }
    status = control(&next, &d, HS_AML_UART_TIOCEXCL, NULL);
    if (status != HS_UART_OK) return fail(&next, status, 0U);
    status = control(&next, &d, HS_AML_UART_TCGETS2, &next.original);
    if (status != HS_UART_OK) return fail(&next, status, 0U);
    next.original_valid = true;
    if (next.original.line != 0U) return fail(&next, HS_UART_CONFIG_UNSUPPORTED, 0U);
    (void)hs_aml_uart_make_profile(&next.original, &config);
    status = control(&next, &d, HS_AML_UART_TCSETS2, &config);
    if (status != HS_UART_OK) return fail(&next, status, 0U);
    memset(&observed, 0, sizeof(observed));
    status = control(&next, &d, HS_AML_UART_TCGETS2, &observed);
    if (status != HS_UART_OK) return fail(&next, status, 0U);
    if (memcmp(&config, &observed, sizeof(config)) != 0)
        return fail(&next, HS_UART_CONFIG_MISMATCH, 0U);
    status = guard(&next, &d);
    if (status != HS_UART_OK) return fail(&next, status, 0U);
    *s = next;
    return result(HS_UART_OK, 0U, 0U);
}

static enum hs_aml_uart_status wait_ready(struct hs_aml_uart_session *s,
                                          struct deadline *d, unsigned requested)
{
    enum hs_aml_uart_status status;
    enum hs_aml_uart_io_error error;
    unsigned events, wait_ms;
    int rc;
    for (;;) {
        status = guard(s, d);
        if (status != HS_UART_OK) return status;
        wait_ms = (unsigned)(d->end - d->last);
        if (wait_ms > HS_AML_UART_POLL_SLICE_MS) wait_ms = HS_AML_UART_POLL_SLICE_MS;
        events = 0U;
        error = HS_UART_IO_NONE;
        rc = s->ops.wait(s->context, s->fd, requested, wait_ms, &events, &error);
        if (rc == -1) {
            if (error == HS_UART_IO_INTERRUPTED || error == HS_UART_IO_AGAIN) continue;
            return HS_UART_IO_ERROR;
        }
        if (rc != 0 && rc != 1) return HS_UART_CALLBACK_ERROR;
        if ((events & ~(unsigned)(HS_UART_READABLE | HS_UART_WRITABLE | HS_UART_ERROR |
                                   HS_UART_HUP | HS_UART_INVALID_FD)) != 0U)
            return HS_UART_CALLBACK_ERROR;
        if (rc == 0 && events != 0U) return HS_UART_CALLBACK_ERROR;
        if ((events & (HS_UART_ERROR | HS_UART_INVALID_FD)) != 0U) return HS_UART_IO_ERROR;
        if ((events & HS_UART_HUP) != 0U) return HS_UART_HANGUP;
        if ((events & requested) != 0U) return HS_UART_OK;
    }
}

static struct hs_aml_uart_result transfer(struct hs_aml_uart_session *s,
                                           uint8_t *read_buffer, const uint8_t *write_buffer,
                                           size_t length, uint32_t timeout_ms, bool writing)
{
    size_t done = 0U;
    ptrdiff_t n;
    struct deadline d;
    enum hs_aml_uart_status status;
    enum hs_aml_uart_io_error error;
    if (s == NULL || length == 0U || length > HS_AML_UART_MAX_BYTES ||
        !hs_span_nonempty(writing ? (const void *)write_buffer : read_buffer, length) ||
        timeout_ms == 0U || timeout_ms > HS_AML_UART_MAX_TIMEOUT_MS)
        return result(HS_UART_ARGUMENT, 0U, 0U);
    if (!s->active) return result(HS_UART_NOT_OPEN, 0U, 0U);
    if (writing && !s->writes_authorized) return result(HS_UART_NOT_READY, 0U, 0U);
    status = begin(s, timeout_ms, &d);
    if (status != HS_UART_OK) return fail(s, status, 0U);
    while (done < length) {
        status = guard(s, &d);
        if (status != HS_UART_OK) return fail(s, status, done);
        error = HS_UART_IO_NONE;
        n = writing ? s->ops.write(s->context, s->fd, write_buffer + done, length - done, &error) :
                      s->ops.read(s->context, s->fd, read_buffer, length, &error);
        if (n < -1 || (n >= 0 && (size_t)n > length - done))
            return fail(s, HS_UART_CALLBACK_ERROR, done);
        if (n > 0) {
            done += (size_t)n;
            if (!writing || done == length) {
                status = guard(s, &d);
                if (status != HS_UART_OK) return fail(s, status, done);
                return result(HS_UART_OK, done, 0U);
            }
            continue;
        }
        if (n == -1 && error == HS_UART_IO_INTERRUPTED) continue;
        if (n == -1 && error != HS_UART_IO_AGAIN) return fail(s, HS_UART_IO_ERROR, done);
        status = wait_ready(s, &d, writing ? HS_UART_WRITABLE : HS_UART_READABLE);
        if (status != HS_UART_OK) return fail(s, status, done);
    }
    return result(HS_UART_OK, done, 0U);
}

struct hs_aml_uart_result hs_aml_uart_read_some(
    struct hs_aml_uart_session *s, uint8_t *buffer, size_t capacity, uint32_t timeout_ms)
{
    return transfer(s, buffer, NULL, capacity, timeout_ms, false);
}

struct hs_aml_uart_result hs_aml_uart_write_all(
    struct hs_aml_uart_session *s, const uint8_t *buffer, size_t length, uint32_t timeout_ms)
{
    return transfer(s, NULL, buffer, length, timeout_ms, true);
}
