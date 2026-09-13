/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_bm1362_probe.h"
#include "hs_span.h"
#include "hs_bm1362_commands.h"
#include "hs_bm1362_integrity.h"
#include <string.h>

static bool valid(const struct hs_aml_bm1362_probe *p)
{
    return hs_span_nonempty(p, sizeof(*p)) && p->self == p;
}
static bool ready(const struct hs_miner_lifecycle *l,
                  const struct hs_aml_bm1362_probe_readiness *r)
{
    return l->abi == HS_MINER_LIFECYCLE_ABI && l->phase == HS_MINER_STARTING &&
        l->intent == HS_MINER_COMMAND_RUN && l->owner_quiescent == 0 &&
        l->attempt_generation != 0 && r->ready_flags == HS_MINER_READY_ALL &&
        r->explicit_probe_opt_in && r->aml_bm1362_binding_verified &&
        r->long_reply_mode_verified && r->rx_boundary_verified && r->tx_cleanup_verified;
}
unsigned hs_aml_bm1362_probe_stop(struct hs_aml_bm1362_probe *p)
{
    if (!valid(p)) return 0;
    if (p->uart != NULL) p->cleanup_errors |= hs_aml_uart_close(p->uart);
    p->active = false;
    p->matched = false;
    p->uart = NULL;
    p->lifecycle = NULL;
    p->readiness = NULL;
    return p->cleanup_errors;
}
static enum hs_aml_bm1362_probe_status gate(struct hs_aml_bm1362_probe *p)
{
    if (!valid(p)) return HS_PROBE_ARGUMENT;
    if (!p->active) return HS_PROBE_CLOSED;
    if (!ready(p->lifecycle, p->readiness) ||
        p->lifecycle->attempt_generation != p->generation || p->uart == NULL ||
        !p->uart->active || p->uart->fd < 0 || !p->uart->writes_authorized) {
        (void)hs_aml_bm1362_probe_stop(p);
        return HS_PROBE_NOT_READY;
    }
    return HS_PROBE_OK;
}
enum hs_aml_bm1362_probe_status hs_aml_bm1362_probe_bind(
    struct hs_aml_bm1362_probe *p, struct hs_aml_uart_session *u,
    const struct hs_miner_lifecycle *l, const struct hs_aml_bm1362_probe_readiness *r)
{
    if (!hs_span_nonempty(p, sizeof(*p)) || !hs_span_nonempty(u, sizeof(*u)) || !hs_span_nonempty(l, sizeof(*l)) ||
        !hs_span_nonempty(r, sizeof(*r)) || (p->self != NULL && p->self != p) ||
        p->active || p->cleanup_errors ||
        hs_spans_overlap(p, sizeof(*p), u, sizeof(*u)) ||
        hs_spans_overlap(p, sizeof(*p), l, sizeof(*l)) ||
        hs_spans_overlap(p, sizeof(*p), r, sizeof(*r)) ||
        hs_spans_overlap(u, sizeof(*u), l, sizeof(*l)) ||
        hs_spans_overlap(u, sizeof(*u), r, sizeof(*r)) ||
        hs_spans_overlap(l, sizeof(*l), r, sizeof(*r))) return HS_PROBE_ARGUMENT;
    if (!ready(l, r) || !u->active || u->fd < 0 || u->chain > 2 ||
        !u->writes_authorized || u->ops.now_ms == NULL || u->ops.cancelled == NULL ||
        (p->self == p && l->attempt_generation <= p->generation)) return HS_PROBE_NOT_READY;
    memset(p, 0, sizeof(*p));
    p->self = p;
    p->uart = u;
    p->lifecycle = l;
    p->readiness = r;
    p->generation = l->attempt_generation;
    p->active = true;
    return HS_PROBE_OK;
}
struct hs_aml_uart_session *hs_aml_bm1362_probe_take_uart(struct hs_aml_bm1362_probe *p)
{
    struct hs_aml_uart_session *u;
    if (gate(p) != HS_PROBE_OK || !p->matched) return NULL;
    u = p->uart;
    p->uart = NULL;
    p->lifecycle = NULL;
    p->readiness = NULL;
    p->active = false;
    return u;
}
static enum hs_aml_bm1362_probe_status tick(struct hs_aml_bm1362_probe *p,
    uint64_t *last, uint64_t end, uint32_t *remaining)
{
    uint64_t now;
    enum hs_aml_bm1362_probe_status s = gate(p);
    if (s != HS_PROBE_OK) return s;
    if (p->uart->ops.cancelled(p->uart->context)) return HS_PROBE_CANCELLED;
    if (!p->uart->ops.now_ms(p->uart->context, &now) || now < *last)
        return HS_PROBE_CLOCK_ERROR;
    *last = now;
    if (now >= end) return HS_PROBE_TIMEOUT;
    *remaining = (uint32_t)(end - now);
    return HS_PROBE_OK;
}

static size_t resync(uint8_t frame[11])
{
    for (size_t i = 1; i < 10; ++i) {
        if (frame[i] == 0xaa && frame[i + 1] == 0x55) {
            memmove(frame, frame + i, 11U - i);
            return 11U - i;
        }
    }
    if (frame[10] == 0xaa) { frame[0] = 0xaa; return 1; }
    return 0;
}
struct hs_aml_bm1362_probe_result hs_aml_bm1362_probe_run(
    struct hs_aml_bm1362_probe *p, uint32_t timeout_ms)
{
    struct hs_aml_bm1362_probe_result out = {0};
    uint8_t wire[7] = {0x55, 0xaa}, frame[11] = {0}, bytes[11];
    uint64_t last, end;
    uint32_t remaining;
    size_t used = 0;
    out.status = gate(p);
    if (out.status != HS_PROBE_OK) goto done;
    if (timeout_ms == 0 || timeout_ms > HS_AML_UART_MAX_TIMEOUT_MS) {
        out.status = HS_PROBE_ARGUMENT;
        goto done;
    }
    if (p->attempted) { out.status = HS_PROBE_ALREADY_ATTEMPTED; goto done; }
    p->attempted = true;
    if (p->uart->ops.cancelled(p->uart->context)) {
        out.status = HS_PROBE_CANCELLED;
        goto fail;
    }
    if (!p->uart->ops.now_ms(p->uart->context, &last) || UINT64_MAX - last < timeout_ms) {
        out.status = HS_PROBE_CLOCK_ERROR;
        goto fail;
    }
    end = last + timeout_ms;
    if (last < p->lifecycle->phase_since_ms) {
        out.status = HS_PROBE_CLOCK_ERROR;
        goto fail;
    }
    uint64_t startup_elapsed = last - p->lifecycle->phase_since_ms;
    if (startup_elapsed >= p->lifecycle->policy.startup_timeout_ms) {
        out.status = HS_PROBE_TIMEOUT;
        goto fail;
    }
    uint64_t startup_remaining = p->lifecycle->policy.startup_timeout_ms - startup_elapsed;
    if (startup_remaining < timeout_ms) end = last + startup_remaining;
    if (hs_bm1362_encode_probe(wire + 2, 5).status != HS_BM1362_COMMAND_OK) {
        out.status = HS_PROBE_ARGUMENT;
        goto fail;
    }
    out.status = tick(p, &last, end, &remaining);
    if (out.status != HS_PROBE_OK) goto fail;
    out.transfer = hs_aml_uart_write_all(p->uart, wire, sizeof(wire), remaining);
    out.tx_bytes = out.transfer.transferred;
    if (out.transfer.status != HS_UART_OK) {
        out.status = HS_PROBE_TRANSPORT_FAILED;
        goto fail;
    }
    for (;;) {
        out.status = tick(p, &last, end, &remaining);
        if (out.status != HS_PROBE_OK) goto fail;
        if (out.rx_bytes >= HS_AML_BM1362_PROBE_MAX_RX_BYTES) {
            out.status = HS_PROBE_BUDGET;
            goto fail;
        }
        size_t capacity = sizeof(frame) - used;
        size_t budget = HS_AML_BM1362_PROBE_MAX_RX_BYTES - out.rx_bytes;
        if (capacity > budget) capacity = budget;
        out.transfer = hs_aml_uart_read_some(p->uart, bytes, capacity, remaining);
        out.rx_bytes += out.transfer.transferred;
        if (out.transfer.status != HS_UART_OK) {
            out.status = HS_PROBE_TRANSPORT_FAILED;
            goto fail;
        }
        out.status = tick(p, &last, end, &remaining);
        if (out.status != HS_PROBE_OK) goto fail;
        for (size_t i = 0; i < out.transfer.transferred; ++i) {
            uint8_t b = bytes[i];
            if (used == 0) {
                if (b == 0xaa) frame[used++] = b;
            } else if (used == 1 && b != 0x55) {
                used = b == 0xaa ? 1U : 0U;
            } else {
                frame[used++] = b;
            }
            if (used != sizeof(frame)) continue;
            struct hs_bm1362_integrity integrity = hs_bm1362_inspect_integrity(frame, 11);
            if (!integrity.crc_valid) {
                ++out.bad_crc_frames;
                used = resync(frame);
            } else if (integrity.status != HS_BM1362_INTEGRITY_OK ||
                       integrity.kind != HS_BM1362_RESPONSE_REGISTER || frame[7] != 4) {
                ++out.other_frames;
                used = 0;
            } else {
                out.chain = p->uart->chain;
                out.chip_address = frame[6];
                out.register_id = frame[7];
                out.opaque[0] = frame[8];
                out.opaque[1] = frame[9];
                out.value = (uint32_t)frame[2] << 24 | (uint32_t)frame[3] << 16 |
                            (uint32_t)frame[4] << 8 | (uint32_t)frame[5];
                memcpy(out.frame, frame, sizeof(frame));
                p->matched = true;
                goto done;
            }
        }
    }
fail:
    if (valid(p)) {
        p->cleanup_errors |= out.transfer.cleanup_errors;
        (void)hs_aml_bm1362_probe_stop(p);
    }
done:
    if (valid(p)) out.transfer.cleanup_errors |= p->cleanup_errors;
    return out;
}
