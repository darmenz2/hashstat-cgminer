/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_setup.h"
#include "hs_span.h"
#include "hs_bm1362_commands.h"
#include "hs_aml_frame.h"

enum { S_READY = 1, S_ACK, S_WAIT, S_DONE, S_FAILED, S_CANCELLED };
enum { WAIT_ASSERT = 1, WAIT_CORE, WAIT_FINAL };
static int valid(const struct hs_bm1362_setup *s)
{
    return hs_span_nonempty(s, sizeof(*s)) && s->self == s &&
        s->state >= S_READY && s->state <= S_CANCELLED &&
        (unsigned)s->stage <= (unsigned)HS_SETUP_STAGE_DONE;
}
static int terminal(const struct hs_bm1362_setup *s)
{
    return s->state == S_DONE || s->state == S_FAILED || s->state == S_CANCELLED;
}
static uint32_t wait_ms(const struct hs_bm1362_setup *s)
{
    if (s->wait_kind == WAIT_ASSERT) return s->config.fast ? 5U : 10U;
    if (s->wait_kind == WAIT_CORE) return s->config.fast ? 1U : 5U;
    return 10U;
}
static struct hs_bm1362_setup_result output(const struct hs_bm1362_setup *s)
{
    struct hs_bm1362_setup_result r = {0};
    if (!valid(s)) return r;
    r.attempt_tag = s->config.attempt_tag;
    r.stage = s->stage;
    r.failure = s->failure;
    r.write_id = s->stage < HS_SETUP_STAGE_DONE ? (unsigned)s->stage + 1U : 0U;
    r.completed_writes = s->completed_writes;
    r.chip_address = (uint8_t)s->config.chip_address;
    r.confirmed_bytes = s->confirmed_bytes;
    r.transport_error = s->transport_error;
    switch (s->state) {
    case S_READY: r.status = HS_SETUP_PENDING; break;
    case S_ACK: r.status = HS_SETUP_ACK_REQUIRED; break;
    case S_WAIT: r.status = HS_SETUP_WAIT; break;
    case S_DONE: r.status = HS_SETUP_TX_COMPLETE; break;
    case S_CANCELLED: r.status = HS_SETUP_CANCELLED; break;
    default: r.status = HS_SETUP_FAILED; break;
    }
    return r;
}
static struct hs_bm1362_setup_result fail(struct hs_bm1362_setup *s,
                                         enum hs_bm1362_setup_failure why)
{
    s->failure = why;
    s->state = why == HS_SETUP_FAILURE_CANCEL ? S_CANCELLED : S_FAILED;
    return output(s);
}
static int tick(struct hs_bm1362_setup *s, uint64_t now)
{
    if (now < s->last_ms) return 0;
    s->last_ms = now;
    return 1;
}
enum hs_bm1362_setup_status hs_bm1362_setup_init(struct hs_bm1362_setup *s,
    const struct hs_bm1362_setup_config *config, uint64_t now)
{
    if (!hs_span_nonempty(s, sizeof(*s)) || !hs_span_nonempty(config, sizeof(*config)) || s->self != NULL)
        return HS_SETUP_ARGUMENT;
    const struct hs_bm1362_setup_config c = *config;
    if (c.attempt_tag == 0 || c.write_timeout_ms == 0 ||
        c.write_timeout_ms > HS_BM1362_SETUP_MAX_TIMEOUT_MS || c.fast > 1U ||
        c.chip_address > 174U || (c.chip_address & 1U) != 0U)
        return HS_SETUP_ARGUMENT;
    const struct hs_bm1362_setup fresh = {
        .self = s, .config = c, .last_ms = now, .since_ms = now,
        .stage = HS_SETUP_ASSERT_A8, .state = S_READY
    };
    *s = fresh;
    return HS_SETUP_PENDING;
}
static void command(const struct hs_bm1362_setup *s, uint8_t *reg, uint32_t *value)
{
    switch (s->stage) {
    case HS_SETUP_ASSERT_A8:
        *reg = 0xa8U; *value = s->config.chain_cached_a8 | UINT32_C(1); break;
    case HS_SETUP_ASSERT_18:
        *reg = 0x18U; *value = (s->config.chain_cached_18 & UINT32_C(0xcfffffff)) | 3U; break;
    case HS_SETUP_CLEAR_A8:
        *reg = 0xa8U; *value = s->config.chain_cached_a8 & UINT32_C(0xfffffffe); break;
    case HS_SETUP_CLEAR_18:
        *reg = 0x18U; *value = s->config.chain_cached_18 | UINT32_C(0xb0000003); break;
    case HS_SETUP_CLOCK_SELECT: *reg = 0x3cU; *value = UINT32_C(0x80008540); break;
    case HS_SETUP_CLOCK_DELAY: *reg = 0x3cU; *value = UINT32_C(0x80008008); break;
    default: *reg = 0x3cU; *value = UINT32_C(0x800082aa); break;
    }
}
struct hs_bm1362_setup_result hs_bm1362_setup_next(struct hs_bm1362_setup *s, uint64_t now)
{
    if (!valid(s) || terminal(s)) return output(s);
    if (!tick(s, now)) return fail(s, HS_SETUP_FAILURE_CLOCK);
    if (s->state == S_ACK) {
        if (now - s->since_ms >= s->config.write_timeout_ms)
            return fail(s, HS_SETUP_FAILURE_DEADLINE);
        struct hs_bm1362_setup_result r = output(s);
        r.remaining_ms = s->config.write_timeout_ms - (uint32_t)(now - s->since_ms);
        return r;
    }
    if (s->state == S_WAIT) {
        const uint32_t duration = wait_ms(s);
        const uint64_t elapsed = now - s->since_ms;
        if (elapsed < duration) {
            struct hs_bm1362_setup_result r = output(s);
            r.remaining_ms = duration - (uint32_t)elapsed;
            return r;
        }
        if (s->wait_kind == WAIT_FINAL) {
            s->stage = HS_SETUP_STAGE_DONE;
            s->state = S_DONE;
            return output(s);
        }
        if (s->wait_kind == WAIT_CORE) {
            s->wait_kind = WAIT_FINAL;
            s->since_ms = now;
            struct hs_bm1362_setup_result r = output(s);
            r.remaining_ms = 10U;
            return r;
        }
        s->state = S_READY;
    }
    struct hs_bm1362_setup_result r = output(s);
    uint8_t payload[HS_BM1362_WRITE_PAYLOAD_BYTES];
    command(s, &r.register_id, &r.register_value);
    struct hs_bm1362_command_result encoded = hs_bm1362_encode_register_write(
        s->config.chip_address, r.register_id, r.register_value, payload, sizeof(payload));
    if (encoded.status != HS_BM1362_COMMAND_OK || encoded.written != sizeof(payload))
        return fail(s, HS_SETUP_FAILURE_ENCODING);
    struct hs_aml_frame_result framed = hs_aml_frame_pack(payload, sizeof(payload), r.frame, sizeof(r.frame));
    if (framed.status != HS_AML_FRAME_OK || framed.frame_bytes != sizeof(r.frame))
        return fail(s, HS_SETUP_FAILURE_ENCODING);
    s->state = S_ACK;
    s->since_ms = now;
    s->confirmed_bytes = 0;
    s->transport_error = 0;
    r.status = HS_SETUP_WRITE;
    r.frame_bytes = framed.frame_bytes;
    r.confirmed_bytes = 0;
    r.transport_error = 0;
    r.remaining_ms = s->config.write_timeout_ms;
    return r;
}
struct hs_bm1362_setup_result hs_bm1362_setup_ack(struct hs_bm1362_setup *s,
    uint64_t now, uint64_t attempt_tag, unsigned write_id,
    enum hs_bm1362_setup_receipt receipt, size_t bytes, int error)
{
    if (!valid(s) || terminal(s)) return output(s);
    s->confirmed_bytes = bytes;
    s->transport_error = error;
    if (!tick(s, now)) return fail(s, HS_SETUP_FAILURE_CLOCK);
    if (s->state != S_ACK || attempt_tag != s->config.attempt_tag ||
        write_id != (unsigned)s->stage + 1U || receipt < HS_SETUP_TRANSFER_COMPLETE ||
        receipt > HS_SETUP_TRANSFER_CANCELLED)
        return fail(s, HS_SETUP_FAILURE_ACK);
    if (receipt == HS_SETUP_TRANSFER_CANCELLED) return fail(s, HS_SETUP_FAILURE_CANCEL);
    if (receipt == HS_SETUP_TRANSFER_FAILED) return fail(s, HS_SETUP_FAILURE_TRANSPORT);
    if (bytes != HS_BM1362_SETUP_FRAME_BYTES) return fail(s, HS_SETUP_FAILURE_PARTIAL);
    if (now - s->since_ms >= s->config.write_timeout_ms)
        return fail(s, HS_SETUP_FAILURE_DEADLINE);
    ++s->completed_writes;
    s->since_ms = now;
    if (s->stage == HS_SETUP_ASSERT_18) {
        s->state = S_WAIT; s->wait_kind = WAIT_ASSERT;
    } else if (s->stage == HS_SETUP_CORE_ENABLE) {
        s->state = S_WAIT; s->wait_kind = WAIT_CORE;
    } else {
        s->state = S_READY;
    }
    if (s->stage != HS_SETUP_CORE_ENABLE)
        s->stage = (enum hs_bm1362_setup_stage)((unsigned)s->stage + 1U);
    struct hs_bm1362_setup_result r = output(s);
    if (s->state == S_WAIT) r.remaining_ms = wait_ms(s);
    return r;
}
struct hs_bm1362_setup_result hs_bm1362_setup_cancel(struct hs_bm1362_setup *s, uint64_t now)
{
    if (!valid(s) || terminal(s)) return output(s);
    if (!tick(s, now)) return fail(s, HS_SETUP_FAILURE_CLOCK);
    return fail(s, HS_SETUP_FAILURE_CANCEL);
}
