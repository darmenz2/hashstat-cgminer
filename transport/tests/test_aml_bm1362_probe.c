/* SPDX-License-Identifier: GPL-3.0-only */



#ifdef NDEBUG
#error "Assertions must remain enabled"
#endif
#define main hs_uart_regressions
#include "test_aml_uart.c"
#undef main
#include "hs_aml_bm1362_probe.h"

static unsigned probe_checks;
#define P(x) do { ++probe_checks; assert(x); } while (0)

struct probe_mock {
    struct mock uart;
    uint8_t stream[256];
    size_t length, offset, chunk;
    bool noise_forever, revoke_after_write, stop_after_read, late_after_read;
    struct hs_miner_lifecycle *life;
    struct hs_aml_bm1362_probe_readiness *readiness;
};
static ptrdiff_t stream_read(void *context, int fd, uint8_t *bytes, size_t length,
                              enum hs_aml_uart_io_error *error)
{
    struct probe_mock *m = context;
    P(fd == 7 && m->uart.written_count == 7);
    ++m->uart.read_calls;
    ++m->uart.callbacks;
    if (m->noise_forever) { memset(bytes, 0, length); return (ptrdiff_t)length; }
    size_t n = m->length - m->offset;
    if (n == 0) { *error = HS_UART_IO_AGAIN; return -1; }
    if (n > length) n = length;
    if (m->chunk != 0 && n > m->chunk) n = m->chunk;
    memcpy(bytes, m->stream + m->offset, n);
    m->offset += n;
    if (m->stop_after_read) m->life->intent = HS_MINER_COMMAND_STOP;
    if (m->late_after_read) m->uart.now += 1000;
    return (ptrdiff_t)n;
}
static ptrdiff_t stream_write(void *context, int fd, const uint8_t *bytes, size_t length,
                               enum hs_aml_uart_io_error *error)
{
    struct probe_mock *m = context;
    ptrdiff_t n = mock_write(&m->uart, fd, bytes, length, error);
    if (m->revoke_after_write) m->readiness->ready_flags &= ~(uint32_t)HS_MINER_READY_FANS;
    return n;
}
static struct hs_miner_lifecycle starting(void)
{
    struct hs_miner_lifecycle l;
    struct hs_miner_lifecycle_policy policy = {1000, 100, 10, 100, 1};
    struct hs_miner_lifecycle_input in = {0};
    P(hs_miner_lifecycle_init(&policy, &l) == HS_MINER_LIFECYCLE_OK);
    in.now_ms = 1000;
    in.command = HS_MINER_COMMAND_RUN;
    in.ready_flags = HS_MINER_READY_ALL;
    struct hs_miner_lifecycle_result r = hs_miner_lifecycle_step(&l, &in);
    P(r.phase == HS_MINER_STARTING && !r.work_permitted);
    return l;
}
static void setup(struct probe_mock *m, struct hs_aml_uart_session *u,
    struct hs_miner_lifecycle *l, struct hs_aml_bm1362_probe_readiness *r,
    struct hs_aml_bm1362_probe *p)
{
    memset(m, 0, sizeof(*m));
    fixture(&m->uart);
    opened(&m->uart, u);
    u->ops.read = stream_read;
    u->ops.write = stream_write;
    *l = starting();
    *r = (struct hs_aml_bm1362_probe_readiness){HS_MINER_READY_ALL, true, true, true, true, true};
    m->life = l;
    m->readiness = r;
    P(hs_aml_bm1362_probe_bind(p, u, l, r) == HS_PROBE_OK);
}
#define BENCH() \
    struct probe_mock m; \
    struct hs_aml_uart_session uart = HS_AML_UART_SESSION_INIT; \
    struct hs_miner_lifecycle life; \
    struct hs_aml_bm1362_probe_readiness readiness; \
    struct hs_aml_bm1362_probe probe = {0}; \
    setup(&m, &uart, &life, &readiness, &probe)



static uint8_t oracle(const uint8_t payload[9])
{
    unsigned char coefficients[77] = {0};
    unsigned residual = 0;
    for (size_t i = 0; i < 5; ++i) coefficients[i] = 1;
    for (size_t i = 0; i < 72; ++i)
        coefficients[i] ^= (unsigned char)(((unsigned)payload[i / 8] >> (7U - (unsigned)(i % 8))) & 1U);
    for (size_t i = 0; i < 72; ++i) {
        if (coefficients[i] != 0) {
            coefficients[i] ^= 1U;
            coefficients[i + 3] ^= 1U;
            coefficients[i + 5] ^= 1U;
        }
    }
    for (size_t i = 72; i < 77; ++i) residual = residual << 1 | coefficients[i];
    return (uint8_t)residual;
}
static void reply(uint8_t frame[11], unsigned address, unsigned reg, unsigned type)
{
    const uint8_t initial[11] = {0xaa, 0x55, 0x12, 0x34, 0x56, 0x78, 0, 0, 0xde, 0xad, 0};
    memcpy(frame, initial, 11);
    frame[6] = (uint8_t)address;
    frame[7] = (uint8_t)reg;
    for (unsigned i = 0; i < 32; ++i) {
        frame[10] = (uint8_t)(type | i);
        if (oracle(frame + 2) == 0) return;
    }
    P(false);
}
static void append(struct probe_mock *m, const uint8_t *bytes, size_t n)
{
    P(n <= sizeof(m->stream) - m->length);
    memcpy(m->stream + m->length, bytes, n);
    m->length += n;
}
static void add_reply(struct probe_mock *m)
{
    uint8_t frame[11];
    reply(frame, 174, 4, 0);
    append(m, frame, 11);
}
static void test_success(void)
{
    for (unsigned chain = 0; chain < 3; ++chain) {
        for (size_t chunk = 1; chunk <= 11; ++chunk) {
            BENCH();

            uart.chain = chain;
            m.chunk = chunk;
            add_reply(&m);
            struct hs_miner_lifecycle before = life;
            P(hs_aml_bm1362_probe_take_uart(&probe) == NULL);
            struct hs_aml_bm1362_probe_result r = hs_aml_bm1362_probe_run(&probe, 100);
            P(r.status == HS_PROBE_OK && r.value == UINT32_C(0x12345678));
            P(r.chain == chain && r.chip_address == 174 && r.register_id == 4);
            P(r.opaque[0] == 0xde && r.opaque[1] == 0xad);
            P(memcmp(r.frame, m.stream, 11) == 0);
            const uint8_t wire[7] = {0x55, 0xaa, 0x52, 5, 0, 4, 0x1e};
            P(r.tx_bytes == 7 && r.rx_bytes == 11 && m.uart.written_count == 7);
            P(memcmp(m.uart.written, wire, 7) == 0);
            P(memcmp(&life, &before, sizeof(life)) == 0 && life.phase == HS_MINER_STARTING);
            P(hs_aml_bm1362_probe_run(&probe, 100).status == HS_PROBE_ALREADY_ATTEMPTED);
            P(hs_aml_bm1362_probe_take_uart(&probe) == &uart && uart.active);
            P(hs_aml_bm1362_probe_take_uart(&probe) == NULL);
            P(hs_aml_bm1362_probe_stop(&probe) == 0 && m.uart.close_calls == 0);
            P(hs_aml_bm1362_probe_bind(&probe, &uart, &life, &readiness) == HS_PROBE_NOT_READY);
            P(hs_aml_uart_close(&uart) == 0 && m.uart.close_calls == 1);
        }
    }
}
static void test_frame_filtering(void)
{
    for (unsigned bit = 16; bit < 88; ++bit) {
        BENCH();
        uint8_t frame[11], noise[] = {0, 0xaa, 0xaa, 0x55, 0x66, 0x77};
        reply(frame, 174, 4, 0);
        frame[bit / 8] ^= (uint8_t)(1U << (bit % 8));
        append(&m, frame, 11);
        reply(frame, 174, 3, 0); append(&m, frame, 11);
        reply(frame, 174, 4, 0x80); append(&m, frame, 11);
        reply(frame, 174, 4, 0x20); append(&m, frame, 11);
        append(&m, noise, sizeof(noise));
        add_reply(&m);
        m.chunk = (bit % 11) + 1U;
        struct hs_aml_bm1362_probe_result r = hs_aml_bm1362_probe_run(&probe, 100);
        P(r.status == HS_PROBE_OK && r.other_frames == 3 && r.bad_crc_frames >= 1);
        P(r.value == UINT32_C(0x12345678) && r.rx_bytes == m.length);
        P(hs_aml_bm1362_probe_stop(&probe) == 0 && m.uart.close_calls == 1);
    }


    for (unsigned address = 0; address < 256; ++address) {
        BENCH();
        uint8_t frame[11]; reply(frame, address, 4, 0); append(&m, frame, 11);
        struct hs_aml_bm1362_probe_result r = hs_aml_bm1362_probe_run(&probe, 100);
        P(r.status == HS_PROBE_OK && r.chip_address == address);
        P(hs_aml_bm1362_probe_stop(&probe) == 0);
    }
}
static void test_no_fans_and_gates(void)
{
    {


        struct mock m; fixture(&m);
        struct hs_aml_uart_session uart = HS_AML_UART_SESSION_INIT;
        struct hs_miner_lifecycle life;
        const struct hs_miner_lifecycle_policy policy = {1000, 100, 10, 100, 1};
        struct hs_aml_bm1362_probe probe = {0};
        struct hs_aml_bm1362_probe_readiness r = {0};
        struct hs_miner_lifecycle_input in = {0};
        P(hs_miner_lifecycle_init(&policy, &life) == HS_MINER_LIFECYCLE_OK);
        in.now_ms = 1000;
        in.command = HS_MINER_COMMAND_RUN;
        in.ready_flags = HS_MINER_READY_CONTROLLER | HS_MINER_READY_CONFIGURATION;
        struct hs_miner_lifecycle_result step = hs_miner_lifecycle_step(&life, &in);
        P(step.phase == HS_MINER_WAITING_HARDWARE && !step.work_permitted);
        P(hs_aml_bm1362_probe_bind(&probe, &uart, &life, &r) == HS_PROBE_NOT_READY);
        P(hs_aml_bm1362_probe_run(&probe, 100).status == HS_PROBE_ARGUMENT);
        P(m.callbacks == 0 && m.open_calls == 0 && m.write_calls == 0 && !uart.active);
    }
    for (unsigned bit = 0; bit < 5; ++bit) {
        BENCH();
        readiness.ready_flags &= ~(UINT32_C(1) << bit);
        unsigned callbacks = m.uart.callbacks;
        P(hs_aml_bm1362_probe_run(&probe, 100).status == HS_PROBE_NOT_READY);
        P(m.uart.write_calls == 0 && m.uart.read_calls == 0 && m.uart.close_calls == 1);
        P(m.uart.callbacks > callbacks);
    }
    for (unsigned which = 0; which < 12; ++which) {
        BENCH();
        P(hs_aml_bm1362_probe_stop(&probe) == 0);
        struct hs_aml_bm1362_probe fresh = {0};
        struct hs_aml_uart_session new_uart = HS_AML_UART_SESSION_INIT;
        fixture(&m.uart);
        opened(&m.uart, &new_uart);
        switch (which) {
        case 0: readiness.explicit_probe_opt_in = false; break;
        case 1: readiness.aml_bm1362_binding_verified = false; break;
        case 2: readiness.long_reply_mode_verified = false; break;
        case 3: readiness.rx_boundary_verified = false; break;
        case 4: readiness.tx_cleanup_verified = false; break;
        case 5: life.phase = HS_MINER_RUNNING; break;
        case 6: life.intent = HS_MINER_COMMAND_STOP; break;
        case 7: life.owner_quiescent = 1; break;
        case 8: life.abi = 0; break;
        case 9: life.attempt_generation = 0; break;
        case 10: new_uart.writes_authorized = false; break;
        default: readiness.ready_flags &= ~(uint32_t)(HS_MINER_READY_HASHBOARDS | HS_MINER_READY_FANS); break;
        }
        unsigned callbacks = m.uart.callbacks;
        P(hs_aml_bm1362_probe_bind(&fresh, &new_uart, &life, &readiness) == HS_PROBE_NOT_READY);
        P(m.uart.callbacks == callbacks && m.uart.written_count == 0 && fresh.self == NULL);
        P(hs_aml_uart_close(&new_uart) == 0);
    }
}
static void test_partial_and_cleanup(void)
{
    for (ptrdiff_t prefix = 0; prefix < 7; ++prefix) {
        BENCH();
        m.uart.action_count = prefix > 0 ? 2U : 1U;
        m.uart.actions[0] = prefix > 0 ? (struct io_action){prefix, HS_UART_IO_NONE} :
                                             (struct io_action){-1, HS_UART_IO_OTHER};
        m.uart.actions[1] = (struct io_action){-1, HS_UART_IO_OTHER};
        m.uart.close_fail = true;
        m.uart.restore_fail = true;
        struct hs_aml_bm1362_probe_result r = hs_aml_bm1362_probe_run(&probe, 100);
        P(r.status == HS_PROBE_TRANSPORT_FAILED && r.transfer.status == HS_UART_IO_ERROR);
        P(r.tx_bytes == (size_t)prefix && r.rx_bytes == 0 && m.uart.read_calls == 0);
        P(r.transfer.cleanup_errors == (HS_UART_CLOSE_FAILED | HS_UART_RESTORE_FAILED));
        P(probe.cleanup_errors == r.transfer.cleanup_errors && m.uart.close_calls == 1);
        P(hs_aml_bm1362_probe_stop(&probe) == r.transfer.cleanup_errors && m.uart.close_calls == 1);
        life.attempt_generation++;
        P(hs_aml_bm1362_probe_bind(&probe, &uart, &life, &readiness) == HS_PROBE_ARGUMENT);
    }
    BENCH();

    m.uart.actions[0] = (struct io_action){2, HS_UART_IO_NONE};
    m.uart.actions[1] = (struct io_action){5, HS_UART_IO_NONE}; m.uart.action_count = 2;
    add_reply(&m);
    P(hs_aml_bm1362_probe_run(&probe, 100).status == HS_PROBE_OK);
    P(m.uart.write_calls == 2 && m.uart.written_count == 7);
    P(hs_aml_bm1362_probe_stop(&probe) == 0);
}
static void test_cancel_deadline_budget(void)
{
    for (unsigned which = 0; which < 10; ++which) {
        BENCH();
        add_reply(&m);
        switch (which) {
        case 0: m.uart.forced_cancel = true; break;
        case 1: m.revoke_after_write = true; break;
        case 2: m.stop_after_read = true; break;
        case 3: m.late_after_read = true; break;
        case 4: m.uart.now = UINT64_MAX - 50U; break;
        case 5: m.uart.fail_clock_at = m.uart.clocks + 1; break;
        case 6: m.uart.backward_at = m.uart.clocks + 2; break;
        case 7: m.noise_forever = true; break;
        case 8: m.length = 0; break;
        default: m.uart.now += life.policy.startup_timeout_ms; break;
        }
        struct hs_aml_bm1362_probe_result r = hs_aml_bm1362_probe_run(&probe, 100);
        P(r.status != HS_PROBE_OK && !probe.matched && !probe.active && !uart.active);
        P(r.value == 0 && r.register_id == 0 && m.uart.close_calls == 1);
        if (which == 0) P(r.status == HS_PROBE_CANCELLED && r.tx_bytes == 0);
        if (which == 1) P(r.status == HS_PROBE_NOT_READY && m.uart.read_calls == 0);
        if (which == 2) P(r.status == HS_PROBE_NOT_READY);
        if (which == 3) P(r.transfer.status == HS_UART_TIMEOUT);
        if (which >= 4 && which <= 6) P(r.status == HS_PROBE_CLOCK_ERROR && r.tx_bytes == 0);
        if (which == 7) P(r.status == HS_PROBE_BUDGET && r.rx_bytes == HS_AML_BM1362_PROBE_MAX_RX_BYTES);
        if (which == 8) P(r.transfer.status == HS_UART_TIMEOUT && m.uart.now <= 1100);
        if (which == 9) P(r.status == HS_PROBE_TIMEOUT && r.tx_bytes == 0);
    }
    BENCH();
    m.length = 0;
    life.policy.startup_timeout_ms = 25;
    struct hs_aml_bm1362_probe_result r = hs_aml_bm1362_probe_run(&probe, 100);
    P(r.status != HS_PROBE_OK && m.uart.now <= 1025 && r.tx_bytes == 7);
}
static void test_arguments_and_handoff(void)
{
    BENCH();
    P(hs_aml_bm1362_probe_run(NULL, 100).status == HS_PROBE_ARGUMENT);
    P(hs_aml_bm1362_probe_run(&probe, 0).status == HS_PROBE_ARGUMENT);
    P(hs_aml_bm1362_probe_run(&probe, 30001).status == HS_PROBE_ARGUMENT);
    P(!probe.attempted && m.uart.write_calls == 0);
    struct hs_aml_bm1362_probe copied = probe;
    P(hs_aml_bm1362_probe_run(&copied, 100).status == HS_PROBE_ARGUMENT);
    P(hs_aml_bm1362_probe_stop(&copied) == 0 && uart.active);
    P(hs_aml_bm1362_probe_bind(&probe, &uart, &life, &readiness) == HS_PROBE_ARGUMENT);
    add_reply(&m);
    P(hs_aml_bm1362_probe_run(&probe, 100).status == HS_PROBE_OK);
    life.attempt_generation++;
    P(hs_aml_bm1362_probe_take_uart(&probe) == NULL && !uart.active && m.uart.close_calls == 1);
    P(hs_aml_bm1362_probe_run(&probe, 100).status == HS_PROBE_CLOSED);
}
int main(void)
{
    P(hs_uart_regressions() == 0);
    test_success();
    test_frame_filtering();
    test_no_fans_and_gates();
    test_partial_and_cleanup();
    test_cancel_deadline_budget();
    test_arguments_and_handoff();
    printf("AML BM1362 startup probe: %u checks passed; mock only, no hardware I/O\n", probe_checks);
    return 0;
}
