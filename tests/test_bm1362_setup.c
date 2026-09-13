/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_setup.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks;
#define C(x) do { ++checks; assert(x); } while (0)
static struct hs_bm1362_setup_config config(unsigned address, unsigned fast)
{
    struct hs_bm1362_setup_config c = {0};
    c.attempt_tag = 123;
    c.chain_cached_18 = UINT32_C(0x0000c100);
    c.write_timeout_ms = 100;
    c.chip_address = address;
    c.fast = fast;
    return c;
}

static uint8_t crc(const uint8_t *p)
{
    unsigned a = 1, b = 1, c = 1, d = 1, e = 1;
    for (size_t i = 0; i < 8; ++i) {
        for (unsigned bit = 128; bit; bit >>= 1U) {
            unsigned f = a ^ ((p[i] & bit) != 0U), ob = b, oc = c, od = d, oe = e;
            a = ob; b = oc; c = od ^ f; d = oe; e = f;
        }
    }
    return (uint8_t)((a << 4) | (b << 3) | (c << 2) | (d << 1) | e);
}
static void check_frame(struct hs_bm1362_setup_result r,
                        const struct hs_bm1362_setup_config *c, unsigned index)
{
    const uint8_t regs[7] = {0xa8,0x18,0xa8,0x18,0x3c,0x3c,0x3c};
    const uint32_t values[7] = {
        c->chain_cached_a8 | 1U,
        (c->chain_cached_18 & ~UINT32_C(0x30000000)) | 3U,
        c->chain_cached_a8 & ~UINT32_C(1),
        c->chain_cached_18 | UINT32_C(0xb0000003),
        UINT32_C(0x80008540),UINT32_C(0x80008008),UINT32_C(0x800082aa)
    };
    uint8_t expected[11] = {0x55,0xaa,0x41,9,0,0,0,0,0,0,0};
    expected[4] = (uint8_t)c->chip_address;
    expected[5] = regs[index];
    for (unsigned i = 0; i < 4; ++i)
        expected[6 + i] = (uint8_t)(values[index] >> (24U - i * 8U));
    expected[10] = crc(expected + 2);
    C(r.status == HS_SETUP_WRITE && r.frame_bytes == 11);
    C(r.write_id == index + 1U && (unsigned)r.stage == index);
    C(r.attempt_tag == c->attempt_tag && r.completed_writes == index);
    C(r.register_id == regs[index] && r.register_value == values[index]);
    C(r.chip_address == c->chip_address && !memcmp(expected, r.frame, sizeof(expected)));
    C(r.failure == HS_SETUP_FAILURE_NONE && r.confirmed_bytes == 0);
}
static uint64_t first_wait(struct hs_bm1362_setup *s, uint64_t now)
{
    const uint32_t delay = s->config.fast ? 5U : 10U;
    struct hs_bm1362_setup_result r = hs_bm1362_setup_next(s, now + delay - 1U);
    C(r.status == HS_SETUP_WAIT && r.remaining_ms == 1 && r.frame_bytes == 0);
    return now + delay;
}
static void sequence(struct hs_bm1362_setup_config cfg, uint64_t now)
{
    struct hs_bm1362_setup s = {0};
    C(hs_bm1362_setup_init(&s, &cfg, now) == HS_SETUP_PENDING);
    for (unsigned i = 0; i < 7; ++i) {
        struct hs_bm1362_setup_result r = hs_bm1362_setup_next(&s, now);
        check_frame(r, &cfg, i);
        r = hs_bm1362_setup_next(&s, now);
        C(r.status == HS_SETUP_ACK_REQUIRED && r.frame_bytes == 0 && r.remaining_ms == 100);
        r = hs_bm1362_setup_ack(&s, now, cfg.attempt_tag, i + 1U,
                              HS_SETUP_TRANSFER_COMPLETE, 11, 91);
        C(r.completed_writes == i + 1U && r.frame_bytes == 0);
        C(r.status == (i == 1 || i == 6 ? HS_SETUP_WAIT : HS_SETUP_PENDING));
        if (i == 1) now = first_wait(&s, now);
    }
    const uint32_t delay = cfg.fast ? 1U : 5U;
    struct hs_bm1362_setup_result r = hs_bm1362_setup_next(&s, now + delay - 1U);
    C(r.status == HS_SETUP_WAIT && r.remaining_ms == 1);
    now += delay;
    r = hs_bm1362_setup_next(&s, now);
    C(r.status == HS_SETUP_WAIT && r.remaining_ms == 10 && r.frame_bytes == 0);
    C(hs_bm1362_setup_next(&s, now + 9).status == HS_SETUP_WAIT);
    r = hs_bm1362_setup_next(&s, now + 10);
    C(r.status == HS_SETUP_TX_COMPLETE && r.completed_writes == 7 && r.write_id == 0);
    C(r.stage == HS_SETUP_STAGE_DONE && r.frame_bytes == 0);
    C(hs_bm1362_setup_next(&s, 0).status == HS_SETUP_TX_COMPLETE);
    C(hs_bm1362_setup_cancel(&s, 0).status == HS_SETUP_TX_COMPLETE);
    C(hs_bm1362_setup_ack(&s, 0, 0, 0, HS_SETUP_TRANSFER_FAILED, 0, 1).status == HS_SETUP_TX_COMPLETE);
    C(hs_bm1362_setup_init(&s, &cfg, now) == HS_SETUP_ARGUMENT);
}
static uint64_t advance_to(struct hs_bm1362_setup *s,
                          const struct hs_bm1362_setup_config *cfg, unsigned index)
{
    uint64_t now = 100;
    C(hs_bm1362_setup_init(s, cfg, now) == HS_SETUP_PENDING);
    for (unsigned i = 0; i <= index; ++i) {
        check_frame(hs_bm1362_setup_next(s, now), cfg, i);
        if (i == index) break;
        (void)hs_bm1362_setup_ack(s, now, cfg->attempt_tag, i + 1U,
                                HS_SETUP_TRANSFER_COMPLETE, 11, 0);
        if (i == 1) now += cfg->fast ? 5U : 10U;
    }
    return now;
}
static void halted(struct hs_bm1362_setup *s, unsigned stage, unsigned completed,
                   enum hs_bm1362_setup_failure reason)
{
    struct hs_bm1362_setup_result r = hs_bm1362_setup_next(s, UINT64_MAX);
    C(r.status == (reason == HS_SETUP_FAILURE_CANCEL ? HS_SETUP_CANCELLED : HS_SETUP_FAILED));
    C(r.frame_bytes == 0 && (unsigned)r.stage == stage && r.completed_writes == completed);
    C(r.failure == reason && r.write_id == stage + 1U);
    C(hs_bm1362_setup_ack(s, UINT64_MAX, s->config.attempt_tag, stage + 1U,
                         HS_SETUP_TRANSFER_COMPLETE, 11, 0).status == r.status);
}
static void failure_tests(void)
{
    for (unsigned stage = 0; stage < 7; ++stage) {
        for (size_t bytes = 0; bytes <= 12; ++bytes) {
            for (unsigned receipt = 0; receipt < 3; ++receipt) {
                if (bytes == 11 && receipt == 0) continue;
                struct hs_bm1362_setup s = {0};
                struct hs_bm1362_setup_config cfg = config(174, 1);
                uint64_t now = advance_to(&s, &cfg, stage);
                struct hs_bm1362_setup_result r = hs_bm1362_setup_ack(&s, now, cfg.attempt_tag,
                    stage + 1U, (enum hs_bm1362_setup_receipt)receipt, bytes, -99);
                enum hs_bm1362_setup_failure why = receipt == 2 ? HS_SETUP_FAILURE_CANCEL :
                    receipt == 1 ? HS_SETUP_FAILURE_TRANSPORT : HS_SETUP_FAILURE_PARTIAL;
                C(r.confirmed_bytes == bytes && r.transport_error == -99);
                halted(&s, stage, stage, why);
            }
        }
        for (unsigned mode = 0; mode < 8; ++mode) {
            struct hs_bm1362_setup s = {0};
            struct hs_bm1362_setup_config cfg = config(2, 0);
            uint64_t now = advance_to(&s, &cfg, stage);
            enum hs_bm1362_setup_failure why = HS_SETUP_FAILURE_ACK;
            switch (mode) {
            case 0: (void)hs_bm1362_setup_ack(&s, now, 999, stage+1U, HS_SETUP_TRANSFER_COMPLETE, 11, 0); break;
            case 1: (void)hs_bm1362_setup_ack(&s, now, cfg.attempt_tag, stage+2U, HS_SETUP_TRANSFER_COMPLETE, 11, 0); break;
            case 2: (void)hs_bm1362_setup_ack(&s, now, cfg.attempt_tag, stage+1U, (enum hs_bm1362_setup_receipt)99, 11, 0); break;
            case 3: (void)hs_bm1362_setup_ack(&s, now+100, cfg.attempt_tag, stage+1U, HS_SETUP_TRANSFER_COMPLETE, 11, 0); why=HS_SETUP_FAILURE_DEADLINE; break;
            case 4: C(hs_bm1362_setup_next(&s, now+99).status==HS_SETUP_ACK_REQUIRED);
                (void)hs_bm1362_setup_next(&s, now+100); why=HS_SETUP_FAILURE_DEADLINE; break;
            case 5: (void)hs_bm1362_setup_ack(&s, now-1, cfg.attempt_tag, stage+1U, HS_SETUP_TRANSFER_COMPLETE, 11, 0); why=HS_SETUP_FAILURE_CLOCK; break;
            case 6: (void)hs_bm1362_setup_next(&s, now-1); why=HS_SETUP_FAILURE_CLOCK; break;
            default: (void)hs_bm1362_setup_cancel(&s, now); why=HS_SETUP_FAILURE_CANCEL; break;
            }
            halted(&s, stage, stage, why);
        }
    }
}
static void inputs_and_edges(void)
{
    struct hs_bm1362_setup_config cfg = config(0, 0);
    struct hs_bm1362_setup s = {0}, unchanged = s;
    C(hs_bm1362_setup_init(NULL, &cfg, 0) == HS_SETUP_ARGUMENT);
    C(hs_bm1362_setup_init(&s, NULL, 0) == HS_SETUP_ARGUMENT);
    C(hs_bm1362_setup_next(&s, 0).status == HS_SETUP_ARGUMENT);
    C(hs_bm1362_setup_ack(&s, 0, 0, 0, HS_SETUP_TRANSFER_COMPLETE, 11, 0).status == HS_SETUP_ARGUMENT);
    C(hs_bm1362_setup_cancel(NULL, 0).status == HS_SETUP_ARGUMENT);
    for (unsigned a = 0; a <= 256; ++a) {
        cfg = config(a, 0); s = unchanged;
        enum hs_bm1362_setup_status expected = a <= 174 && !(a & 1U) ? HS_SETUP_PENDING : HS_SETUP_ARGUMENT;
        C(hs_bm1362_setup_init(&s, &cfg, 0) == expected);
        if (expected == HS_SETUP_ARGUMENT) C(!memcmp(&s, &unchanged, sizeof(s)));
    }
    for (unsigned which = 0; which < 5; ++which) {
        cfg = config(0, 0); s = unchanged;
        switch (which) {
        case 0: cfg.attempt_tag = 0; break;
        case 1: cfg.write_timeout_ms = 0; break;
        case 2: cfg.write_timeout_ms = 30001; break;
        case 3: cfg.fast = 2; break;
        default: cfg.chip_address = UINT_MAX; break;
        }
        C(hs_bm1362_setup_init(&s, &cfg, 0) == HS_SETUP_ARGUMENT);
        C(!memcmp(&s, &unchanged, sizeof(s)));
    }
    cfg = config(0, 0); s = unchanged;
    C(hs_bm1362_setup_init(&s, &cfg, 10) == HS_SETUP_PENDING);
    struct hs_bm1362_setup copied = s;
    C(hs_bm1362_setup_next(&copied, 10).status == HS_SETUP_ARGUMENT);
    C(hs_bm1362_setup_cancel(&copied, 10).status == HS_SETUP_ARGUMENT);
    (void)hs_bm1362_setup_ack(&s, 10, 123, 1, HS_SETUP_TRANSFER_COMPLETE, 11, 0);
    halted(&s, 0, 0, HS_SETUP_FAILURE_ACK);
    s = unchanged;
    uint64_t now = advance_to(&s, &cfg, 0);
    (void)hs_bm1362_setup_ack(&s, now, 123, 1, HS_SETUP_TRANSFER_COMPLETE, 11, 0);
    (void)hs_bm1362_setup_ack(&s, now, 123, 1, HS_SETUP_TRANSFER_COMPLETE, 11, 0);
    halted(&s, 1, 1, HS_SETUP_FAILURE_ACK);
    s = unchanged;
    (void)hs_bm1362_setup_init(&s, &cfg, UINT64_MAX);
    C(hs_bm1362_setup_next(&s, UINT64_MAX).status == HS_SETUP_WRITE);
    C(hs_bm1362_setup_next(&s, 0).failure == HS_SETUP_FAILURE_CLOCK);

    cfg = config(2, 0); s = unchanged;
    now = advance_to(&s, &cfg, 3);
    (void)hs_bm1362_setup_ack(&s, now, 123, 4, HS_SETUP_TRANSFER_COMPLETE, 11, 0);
    const uint8_t fixed[11] = {0x55,0xaa,0x41,9,2,0x3c,0x80,0,0x85,0x40,0x1b};
    struct hs_bm1362_setup_result fixed_result = hs_bm1362_setup_next(&s, now);
    C(fixed_result.status == HS_SETUP_WRITE && !memcmp(fixed_result.frame, fixed, 11));
    s = unchanged;
    (void)hs_bm1362_setup_init(&s, &cfg, 10);
    (void)hs_bm1362_setup_cancel(&s, 10);
    halted(&s, 0, 0, HS_SETUP_FAILURE_CANCEL);

    for (unsigned mode = 0; mode < 3; ++mode) {
        s = unchanged; now = advance_to(&s, &cfg, 1);
        (void)hs_bm1362_setup_ack(&s, now, 123, 2, HS_SETUP_TRANSFER_COMPLETE, 11, 0);
        if (mode == 0) (void)hs_bm1362_setup_cancel(&s, now);
        else if (mode == 1) (void)hs_bm1362_setup_next(&s, now-1);
        else (void)hs_bm1362_setup_ack(&s, now, 123, 2, HS_SETUP_TRANSFER_COMPLETE, 11, 0);
        halted(&s, 2, 2, mode == 0 ? HS_SETUP_FAILURE_CANCEL : mode == 1 ? HS_SETUP_FAILURE_CLOCK : HS_SETUP_FAILURE_ACK);
    }
}
int main(void)
{
    for (unsigned address = 0; address <= 174; address += 2) {
        for (unsigned fast = 0; fast < 2; ++fast) {
            struct hs_bm1362_setup_config c = config(address, fast);
            sequence(c, 100);
            for (unsigned bit = 0; bit < 32; ++bit) {
                c.chain_cached_a8 = UINT32_C(1) << bit;
                c.chain_cached_18 = ~(UINT32_C(1) << bit);
                sequence(c, 100);
            }
        }
    }
    sequence(config(2,0), UINT64_MAX - 1000U);
    failure_tests(); inputs_and_edges();
    printf("BM1362 setup PASS: %lu checks; planner only; no hardware\n", checks);
    return 0;
}
