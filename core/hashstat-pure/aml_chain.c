/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_chain.h"
#include "hs_span.h"
#include "hs_btm_work_wire.h"
#include <string.h>

static int valid(const struct hs_aml_chain *c)
{
    return hs_span_nonempty(c, sizeof(*c)) && c->self == c;
}
static struct hs_aml_chain_event event(enum hs_aml_chain_status status)
{
    struct hs_aml_chain_event e = {0};
    e.status = status;
    return e;
}
static struct hs_aml_chain_event gated_event(struct hs_aml_chain *c,
                                            enum hs_aml_chain_status status)
{
    struct hs_aml_chain_event e = event(status);
    if (valid(c)) e.transfer.cleanup_errors = c->cleanup_errors;
    return e;
}
static int ready(const struct hs_miner_lifecycle *l)
{
    return hs_span_nonempty(l, sizeof(*l)) && l->abi == HS_MINER_LIFECYCLE_ABI &&
        l->phase == HS_MINER_RUNNING && l->intent == HS_MINER_COMMAND_RUN &&
        l->owner_quiescent == 0 && l->attempt_generation != 0;
}
unsigned hs_aml_chain_stop(struct hs_aml_chain *c)
{
    unsigned cleanup = 0;
    if (!valid(c)) return 0;
    c->active = 0;
    c->used = 0;
    (void)hs_job_cache_clear(&c->jobs, c->jobs.session_tag);
    if (c->uart != NULL) cleanup = hs_aml_uart_close(c->uart);
    c->uart = NULL;
    c->lifecycle = NULL;
    c->cleanup_errors |= cleanup;
    return c->cleanup_errors;
}
static enum hs_aml_chain_status gate(struct hs_aml_chain *c)
{
    if (!valid(c)) return HS_CHAIN_ARGUMENT;
    if (c->active != 1) return HS_CHAIN_CLOSED;
    if (!ready(c->lifecycle) || c->lifecycle->attempt_generation != c->generation ||
        c->uart == NULL || !c->uart->active || !c->uart->writes_authorized) {
        (void)hs_aml_chain_stop(c);
        return HS_CHAIN_NOT_READY;
    }
    return HS_CHAIN_OK;
}
static int clock_tick(struct hs_aml_chain *c, uint64_t now)
{
    if (c->clock_started && now < c->last_ms) return 0;
    c->clock_started = 1;
    c->last_ms = now;
    return 1;
}
enum hs_aml_chain_status hs_aml_chain_bind(struct hs_aml_chain *c,
    struct hs_aml_uart_session *uart, const struct hs_miner_lifecycle *l,
    uint64_t session_tag)
{
    if (!hs_span_nonempty(c, sizeof(*c)) || !hs_span_nonempty(uart, sizeof(*uart)) || !hs_span_nonempty(l, sizeof(*l)) ||
        (c->self != NULL && c->self != c) || c->active || c->cleanup_errors || session_tag == 0 ||
        hs_spans_overlap(c, sizeof(*c), uart, sizeof(*uart)) ||
        hs_spans_overlap(c, sizeof(*c), l, sizeof(*l)) ||
        hs_spans_overlap(uart, sizeof(*uart), l, sizeof(*l))) return HS_CHAIN_ARGUMENT;
    if (!ready(l) || !uart->active || !uart->writes_authorized || uart->fd < 0 ||
        uart->chain > 2 || uart->ops.now_ms == NULL) return HS_CHAIN_NOT_READY;

    if (c->self == c && l->attempt_generation <= c->generation)
        return HS_CHAIN_NOT_READY;
    memset(c, 0, sizeof(*c));
    c->self = c;
    c->uart = uart;
    c->lifecycle = l;
    c->generation = l->attempt_generation;
    c->active = 1;
    (void)hs_job_cache_init(&c->jobs, session_tag);
    return HS_CHAIN_OK;
}
enum hs_aml_chain_status hs_aml_chain_reset_jobs(struct hs_aml_chain *c,
                                              uint64_t session_tag)
{
    enum hs_aml_chain_status s = gate(c);
    if (s != HS_CHAIN_OK) return s;
    if (session_tag == 0 || session_tag < c->jobs.session_tag) return HS_CHAIN_ARGUMENT;
    c->used = 0;
    (void)hs_job_cache_init(&c->jobs, session_tag);
    return HS_CHAIN_OK;
}
struct hs_aml_chain_event hs_aml_chain_send(struct hs_aml_chain *c, unsigned slot,
    const struct hs_job_snapshot *snapshot, uint64_t now, uint32_t timeout_ms)
{
    enum hs_aml_chain_status s = gate(c);
    struct hs_aml_chain_event e;
    uint8_t prefix[76], wire[88];
    uint64_t completed;
    if (s != HS_CHAIN_OK) return gated_event(c, s);
    if (slot >= HS_JOB_CACHE_SLOTS || !hs_span_nonempty(snapshot, sizeof(*snapshot)) ||
        timeout_ms == 0 || timeout_ms > HS_AML_UART_MAX_TIMEOUT_MS)
        return event(HS_CHAIN_ARGUMENT);

    const struct hs_job_snapshot job = *snapshot;
    e = event(HS_CHAIN_JOB_REJECTED);
    e.job_status = hs_job_snapshot_validate(&job, c->jobs.session_tag);
    if (e.job_status != HS_JOB_OK) return e;
    if (!clock_tick(c, now)) {
        e = event(HS_CHAIN_CLOCK_ERROR);
        e.transfer.cleanup_errors = hs_aml_chain_stop(c);
        return e;
    }
    if (now < job.issued_ms || now - job.issued_ms >= job.max_age_ms) {
        e.job_status = HS_JOB_STALE;
        return e;
    }
    if (hs_btm_work_ring_from_header80(job.header, 80, prefix, sizeof(prefix)).status != HS_WORK_WIRE_OK ||
        hs_btm_work_wire_pack(prefix, sizeof(prefix), slot, wire, sizeof(wire)).status != HS_WORK_WIRE_OK)
        return event(HS_CHAIN_ARGUMENT);
    (void)hs_job_cache_retire(&c->jobs, slot);
    e = event(HS_CHAIN_TRANSPORT_FAILED);
    e.transfer = hs_aml_uart_write_all(c->uart, wire, sizeof(wire), timeout_ms);
    c->cleanup_errors |= e.transfer.cleanup_errors;
    if (e.transfer.status != HS_UART_OK || e.transfer.transferred != sizeof(wire)) {
        e.transfer.cleanup_errors |= hs_aml_chain_stop(c);
        return e;
    }

    if (!c->uart->ops.now_ms(c->uart->context, &completed) || !clock_tick(c, completed)) {
        e.status = HS_CHAIN_CLOCK_ERROR;
        e.transfer.cleanup_errors |= hs_aml_chain_stop(c);
        return e;
    }
    if (completed < job.issued_ms || completed - job.issued_ms >= job.max_age_ms) {
        e.status = HS_CHAIN_JOB_REJECTED;
        e.job_status = HS_JOB_STALE;
        return e;
    }
    s = gate(c);
    if (s != HS_CHAIN_OK) {
        e.status = s;
        e.transfer.cleanup_errors |= c->cleanup_errors;
        return e;
    }
    e.job_status = hs_job_cache_publish(&c->jobs, slot, &job);
    if (e.job_status != HS_JOB_OK) {
        e.status = HS_CHAIN_JOB_REJECTED;
        e.transfer.cleanup_errors |= hs_aml_chain_stop(c);
        return e;
    }
    e.status = HS_CHAIN_OK;
    return e;
}
static void resync(struct hs_aml_chain *c)
{
    for (unsigned start = 1; start < 10; ++start) {
        if (c->frame[start] == 0xaa && c->frame[start + 1U] == 0x55) {
            c->used = (uint8_t)(11U - start);
            memmove(c->frame, c->frame + start, c->used);
            return;
        }
    }
    c->used = c->frame[10] == 0xaa ? 1 : 0;
    if (c->used) c->frame[0] = 0xaa;
}
struct hs_aml_chain_event hs_aml_chain_push(struct hs_aml_chain *c, uint8_t byte,
                                          uint64_t now)
{
    enum hs_aml_chain_status s = gate(c);
    struct hs_aml_chain_event e;
    if (s != HS_CHAIN_OK) return gated_event(c, s);
    if (!clock_tick(c, now)) {
        e = event(HS_CHAIN_CLOCK_ERROR);
        e.transfer.cleanup_errors = hs_aml_chain_stop(c);
        return e;
    }
    if (c->used >= 11 || (c->used && c->frame[0] != 0xaa) ||
        (c->used > 1 && c->frame[1] != 0x55)) {
        e = event(HS_CHAIN_ARGUMENT);
        e.transfer.cleanup_errors = hs_aml_chain_stop(c);
        return e;
    }
    if (c->used == 0) {
        if (byte == 0xaa) c->frame[c->used++] = byte;
        return event(HS_CHAIN_NEED_MORE);
    }
    if (c->used == 1) {
        if (byte == 0x55) c->frame[c->used++] = byte;
        else if (byte != 0xaa) c->used = 0;
        return event(HS_CHAIN_NEED_MORE);
    }
    c->frame[c->used++] = byte;
    if (c->used != 11) return event(HS_CHAIN_NEED_MORE);
    struct hs_bm1362_integrity integrity = hs_bm1362_inspect_integrity(c->frame, 11);
    if (integrity.status != HS_BM1362_INTEGRITY_OK) {
        resync(c);
        return event(integrity.crc_valid ? HS_CHAIN_BAD_TYPE : HS_CHAIN_BAD_CRC);
    }
    if (integrity.kind == HS_BM1362_RESPONSE_REGISTER) {
        e = event(HS_CHAIN_REGISTER);
        memcpy(e.register_frame, c->frame, 11);
        c->used = 0;
        return e;
    }
    struct hs_bm1362_rx_result decoded = hs_bm1362_rx_decode_long(c->frame, 11);
    if (decoded.status != HS_BM1362_RX_DECODED_UNVERIFIED) {
        resync(c);
        return event(HS_CHAIN_BAD_CHIP);
    }
    c->used = 0;
    e = event(HS_CHAIN_SHARE_REJECTED);
    e.job_status = hs_job_cache_check(&c->jobs, &decoded, now, &e.share);
    if (e.job_status == HS_JOB_OK) e.status = HS_CHAIN_SHARE;
    return e;
}
