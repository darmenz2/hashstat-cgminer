

#include "hs_measurement.h"
#include "hs_chip_frequencies.h"
#include "hs_aml_frame.h"
#include "hs_pow.h"
#include "hs_work.h"
#include "hs_btm_work_wire.h"
#include "hs_bm1362_rx.h"
#include "hs_job_cache.h"
#include "hs_bm1362_commands.h"
#include "hs_bm1362_pll.h"
#include "hs_miner_lifecycle.h"
#include "hs_aml_observe.h"
#include <float.h>

const uint32_t hs_arm_probe_case_count = 76;

static void fixture_hex(const char *text, uint8_t *bytes, unsigned length)
{
    for (unsigned i = 0; i < length; ++i) {
        const char a = text[i * 2U], b = text[i * 2U + 1U];
        const unsigned high = a <= '9' ? (unsigned)(a - '0') : (unsigned)(a - 'a') + 10U;
        const unsigned low = b <= '9' ? (unsigned)(b - '0') : (unsigned)(b - 'a') + 10U;
        bytes[i] = (uint8_t)((high << 4) | low);
    }
}
int hs_arm_probe(void);
int hs_arm_probe(void)
{
    double out = 123.0;
    hs_sweep_chip sweep[3] = {{10, 100.0, 0}, {90, 100.0, 0}, {0, 0.0, 1}};
    hs_flagged_value values[3] = {{10.0, 0}, {30.0, 0}, {500.0, 1}};
    int32_t frequencies[6] = {99, 99, 99, 99, 99, 99};
    const char input[] = "::100:500:900:bad:2147483648:extra";
    struct hs_chip_frequencies_result parsed;
    const uint8_t payload[3] = {0x01, 0x55, 0xaa};
    uint8_t frame[6] = {0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
    struct hs_aml_frame_result packed;
    if (hs_sweep_chip_percent(10, 100.0, &out) != HS_OK || out != 10.0) return 1;
    if (hs_sweep_chain_percent(sweep, 3, &out) != HS_OK || out != 50.0) return 2;
    if (hs_sweep_chip_percent(1, 0.001, &out) != HS_OK || out != 100000.0) return 3;
    out = 123.0;
    if (hs_sweep_chain_percent(NULL, 0, &out) != HS_NO_DATA || out != 123.0) return 4;
    if (hs_nonce_chip_measured(1, 1, 1, &out) != HS_OK || out != 4.294967295) return 5;
    if (hs_nonce_chain_measured(100.0, 2, values, 3, &out) != HS_OK || out != 90.0) return 6;
    if (hs_nonce_chain_percent(20.0, values, 3, &out) != HS_OK || out != 50.0) return 7;
    if (hs_nonce_chip_percent(100.0, 0.0, &out) != HS_OK || out != 0.0) return 8;
    if (hs_nonce_chip_percent(-10.0, -20.0, &out) != HS_OK || out != 50.0) return 9;
    out = 123.0;
    if (hs_nonce_chip_measured(1, 1, 0, &out) != HS_INVALID || out != 123.0) return 10;
    if (hs_nonce_chip_percent(DBL_MAX, 1.0, &out) != HS_NONFINITE || out != 123.0) return 11;
    parsed = hs_chip_frequencies_decode(input, sizeof(input) - 1, frequencies, 6, 5, 400, 600);
    if (parsed.status != HS_CHIP_FREQUENCIES_OK || parsed.parsed_tokens != 5 ||
        parsed.zero_tokens != 2 || parsed.clamped_low != 1 || parsed.clamped_high != 1 ||
        parsed.ignored_tokens != 1) return 12;
    if (frequencies[0] != 400 || frequencies[1] != 500 || frequencies[2] != 600 ||
        frequencies[3] != 0 || frequencies[4] != 0 || frequencies[5] != 99) return 13;
    parsed = hs_chip_frequencies_decode(input, sizeof(input) - 1, frequencies, 1, 5, 400, 600);
    if (parsed.status != HS_CHIP_FREQUENCIES_DESTINATION_TOO_SMALL || frequencies[0] != 400) return 14;
    parsed = hs_chip_frequencies_decode(NULL, 0, frequencies, 6, 5, 400, 600);
    if (parsed.status != HS_CHIP_FREQUENCIES_OK || frequencies[0] != 0 || frequencies[4] != 0 || frequencies[5] != 99) return 15;
    packed = hs_aml_frame_pack(payload, 3, frame, 6);
    if (packed.status != HS_AML_FRAME_OK || packed.frame_bytes != 5 ||
        frame[0] != 0x55 || frame[1] != 0xaa || frame[2] != 0x01 ||
        frame[3] != 0x55 || frame[4] != 0xaa || frame[5] != 0x99) return 16;
    packed = hs_aml_frame_pack(frame, 3, frame, 6);
    if (packed.status != HS_AML_FRAME_OVERLAPPING_BUFFERS || packed.frame_bytes != 0 || frame[0] != 0x55) return 17;
    packed = hs_aml_frame_pack(payload, 3, frame, 4);
    if (packed.status != HS_AML_FRAME_DESTINATION_TOO_SMALL || frame[4] != 0xaa) return 18;
    packed = hs_aml_frame_pack(payload, SIZE_MAX, frame, SIZE_MAX);
    if (packed.status != HS_AML_FRAME_SIZE_OVERFLOW || frame[0] != 0x55) return 19;
    packed = hs_aml_frame_pack(NULL, 0, frame, 6);
    if (packed.status != HS_AML_FRAME_OK || packed.frame_bytes != 2 || frame[2] != 0x01) return 20;
    uint8_t hash[32], target[32], prefix[76], wire[88], scratch[8];
    static const uint8_t abc_hash[32] = {
        0x4f,0x8b,0x42,0xc2,0x2d,0xd3,0x72,0x9b,0x51,0x9b,0xa6,0xf6,0x8d,0x2d,0xa7,0xcc,
        0x5b,0x2d,0x60,0x6d,0x05,0xda,0xed,0x5a,0xd5,0x12,0x8c,0xc0,0x3e,0x6c,0x63,0x58};
    hs_work_job job = {0};
    hs_work_result work;
    int meets = -1;
    uint32_t compact = 0;
    if (hs_sha256d((const uint8_t *)"abc", 3U, hash) != HS_POW_OK) return 21;
    for (unsigned i = 0; i < 32U; ++i) if (hash[i] != abc_hash[i]) return 22;
    if (hs_pow_target_from_compact(UINT32_C(0x1d00ffff), target) != HS_POW_OK ||
        target[26] != 0xff || target[27] != 0xff || target[28] != 0) return 23;
    if (hs_pow_compact_from_target(target, &compact) != HS_POW_OK || compact != UINT32_C(0x1d00ffff)) return 24;
    job.coinbase1 = (hs_bytes){(const uint8_t *)"abc", 3U};
    job.version = UINT32_C(0x12345678); job.nbits = UINT32_C(0x1d00ffff);
    job.nonce = UINT32_C(0xa1b2c3d4);
    if (hs_work_build(&job, scratch, sizeof(scratch), &work) != HS_WORK_OK ||
        work.header[0] != 0x78 || work.header[3] != 0x12 ||
        work.header[76] != 0xd4 || work.header[79] != 0xa1) return 25;
    for (unsigned i = 0; i < 32U; ++i)
        if (work.merkle_root[i] != abc_hash[i] || work.header[36U+i] != abc_hash[i]) return 26;
    for (unsigned i = 0; i < 32U; ++i) target[i] = 0xff;
    if (hs_work_check_nonce(work.header, 80U, 0, target, hash, &meets) != HS_WORK_OK || meets != 1) return 27;
    for (unsigned i = 0; i < 76U; ++i) prefix[i] = (uint8_t)i;
    struct hs_work_wire_result encoded = hs_btm_work_wire_pack(prefix, 76U, 31U, wire, 88U);
    if (encoded.status != HS_WORK_WIRE_OK || encoded.written != 88U ||
        wire[0] != 0x55 || wire[1] != 0xaa || wire[4] != 0xf8 ||
        wire[86] != 0xfc || wire[87] != 0x72) return 28;
    struct hs_wire_crc_result crc = hs_wire_crc16_1021((const uint8_t *)"123456789", 9U, 0xffff);
    if (crc.status != HS_WORK_WIRE_OK || crc.crc != 0x29b1) return 29;
    if (hs_work_prevhash_from_stratum(prefix, 32U, hash) != HS_WORK_OK ||
        hash[0] != 3 || hash[3] != 0 || hash[31] != 28) return 30;
    work.header[0] = 0xac; job.nbits = 0;
    if (hs_work_build(&job, scratch, sizeof(scratch), &work) != HS_WORK_INVALID_NBITS ||
        work.header[0] != 0xac) return 31;
    struct hs_bm1362_pll_result pll;
    if (hs_bm1362_pll_for_frequency(645, &pll) != HS_BM1362_PLL_OK ||
        pll.register_value != UINT32_C(0x50670111) ||
        pll.achieved_frequency_units != 643.75) return 32;
    if (hs_bm1362_pll_for_frequency(0, &pll) != HS_BM1362_PLL_INVALID_FREQUENCY ||
        pll.register_value != UINT32_C(0x50670111)) return 33;
    uint8_t command[9];
    struct hs_bm1362_command_result cmd = hs_bm1362_encode_probe(command, sizeof(command));
    if (cmd.status != HS_BM1362_COMMAND_OK || cmd.written != 5 ||
        command[0] != 0x52 || command[1] != 5 || command[2] != 0 || command[3] != 4) return 34;
    cmd = hs_bm1362_encode_register_write(2, 8, pll.register_value, command, sizeof(command));
    if (cmd.status != HS_BM1362_COMMAND_OK || cmd.written != 9 ||
        command[0] != 0x41 || command[2] != 2 || command[3] != 8 ||
        command[4] != 0x50 || command[5] != 0x67 || command[6] != 1 || command[7] != 0x11) return 35;
    cmd = hs_bm1362_encode_address_assignment(256, command, sizeof(command));
    if (cmd.status != HS_BM1362_COMMAND_INVALID_ADDRESS || command[0] != 0x41) return 36;
    const uint8_t rxframe[11] = {0xaa,0x55,0x7c,0x2b,0xac,0x1d,0,0x18,0,0,0x80};
    struct hs_bm1362_rx_result rx = hs_bm1362_rx_decode_long(rxframe, sizeof(rxframe));
    if (rx.status != HS_BM1362_RX_DECODED_UNVERIFIED ||
        rx.nonce != UINT32_C(0x1dac2b7c) || rx.internal_nonce != UINT32_C(0x7c2bac1d) ||
        rx.slot != 3 || rx.version_bits != 0) return 37;
    struct hs_bm1362_rx_stream stream = {{0},0};
    for (unsigned i = 0; i < 11U; ++i) rx = hs_bm1362_rx_stream_push(&stream, rxframe[i]);
    if (rx.status != HS_BM1362_RX_DECODED_UNVERIFIED || rx.slot != 3 || stream.used != 0) return 38;
    struct hs_job_cache cache;
    struct hs_job_snapshot snapshot = {0};
    struct hs_checked_share share;
    if (hs_job_cache_init(&cache, 55) != HS_JOB_OK) return 39;
    job.nbits = UINT32_C(0x1d00ffff); job.version = 1;
    if (hs_work_build(&job, scratch, sizeof(scratch), &work) != HS_WORK_OK) return 40;
    for (unsigned i = 0; i < 80U; ++i) snapshot.header[i] = work.header[i];
    for (unsigned i = 0; i < 32U; ++i) snapshot.share_target_le[i] = 0xff;
    snapshot.session_tag = 55; snapshot.job_tag = 66; snapshot.work_tag = 77;
    snapshot.issued_ms = 1000; snapshot.max_age_ms = 100;
    if (hs_job_cache_publish(&cache, 3, &snapshot) != HS_JOB_OK) return 41;
    if (hs_job_cache_publish(&cache, 3, &snapshot) != HS_JOB_SLOT_ACTIVE) return 42;
    if (hs_job_cache_check(&cache, &rx, 1050, &share) != HS_JOB_OK ||
        share.work_tag != 77 || share.nonce != UINT32_C(0x1dac2b7c) || share.full_version != 1) return 43;
    if (hs_job_cache_check(&cache, &rx, 1050, &share) != HS_JOB_DUPLICATE) return 44;
    if (hs_job_cache_check(&cache, &rx, 1100, &share) != HS_JOB_STALE) return 45;
    if (hs_job_cache_retire(&cache, 3) != HS_JOB_OK) return 46;
    if (hs_job_cache_check(&cache, &rx, 1050, &share) != HS_JOB_EMPTY) return 47;
    encoded = hs_btm_work_ring_from_header80(work.header, 80, prefix, 76);
    if (encoded.status != HS_WORK_WIRE_OK || encoded.written != 76) return 48;
    for (unsigned word = 0; word < 19U; ++word) {
        unsigned source = word < 16U ? 15U - word : 34U - word;
        for (unsigned byte = 0; byte < 4U; ++byte)
            if (prefix[word * 4U + byte] != work.header[source * 4U + byte]) return 49;
    }
    if (hs_job_cache_clear(&cache, 99) != HS_JOB_WRONG_SESSION) return 50;
    const struct hs_miner_lifecycle_policy policy = {100,20,10,25,3};
    struct hs_miner_lifecycle lifecycle;
    struct hs_miner_lifecycle_input event = {0, HS_MINER_COMMAND_RUN, 0, HS_MINER_OWNER_NONE, 0};
    if (hs_miner_lifecycle_init(&policy, &lifecycle) != HS_MINER_LIFECYCLE_OK ||
        lifecycle.phase != HS_MINER_STOPPED) return 51;
    struct hs_miner_lifecycle_result state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.status != HS_MINER_LIFECYCLE_OK || state.phase != HS_MINER_WAITING_HARDWARE ||
        state.work_permitted) return 52;
    event.command = HS_MINER_COMMAND_NONE; event.ready_flags = HS_MINER_READY_ALL; event.now_ms = 1;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_STARTING || state.action_generation != 1 ||
        !(state.actions & HS_MINER_ACTION_REQUEST_START) || state.work_permitted) return 53;
    event.owner_event = HS_MINER_OWNER_READY; event.owner_generation = 0;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_STARTING || !state.owner_event_ignored || state.work_permitted) return 54;
    event.owner_generation = 1;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_RUNNING || !state.work_permitted) return 55;
    event.command = HS_MINER_COMMAND_STOP;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_DRAINING || state.work_permitted) return 56;
    event.command = HS_MINER_COMMAND_NONE;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_DRAINING || state.work_permitted) return 57;
    event.owner_event = HS_MINER_OWNER_DRAINED;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_STOPPED || state.work_permitted) return 58;
    event.owner_event = HS_MINER_OWNER_NONE; event.now_ms = 9000;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_STOPPED || state.work_permitted) return 59;
    event.command = HS_MINER_COMMAND_RUN;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_STARTING || state.action_generation != 2) return 60;
    event.command = HS_MINER_COMMAND_NONE; event.now_ms = 8999;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.status != HS_MINER_LIFECYCLE_TIME_REVERSED ||
        lifecycle.phase != HS_MINER_FAULTED || state.work_permitted) return 61;
    event.now_ms = 9001; event.owner_event = HS_MINER_OWNER_READY; event.owner_generation = 2;
    state = hs_miner_lifecycle_step(&lifecycle, &event);
    if (state.phase != HS_MINER_FAULTED || state.work_permitted) return 62;
    if (!hs_aml_presence_direction_is_input((const uint8_t *)"in\n", 3, true)) return 63;
    if (!hs_aml_presence_active_low_is_disabled((const uint8_t *)"0\n", 2, true)) return 64;
    if (hs_aml_presence_decode((const uint8_t *)"1\n", 2, true, true, true) != HS_AML_PRESENCE_PRESENT) return 65;
    if (hs_aml_presence_decode((const uint8_t *)"1\n", 2, false, true, true) != HS_AML_PRESENCE_UNKNOWN ||
        hs_aml_presence_decode((const uint8_t *)"1\n", 2, true, true, false) != HS_AML_PRESENCE_UNKNOWN) return 66;
    uint32_t gpio = 0;
    if (!hs_aml_presence_gpio_for_chain(2, &gpio) || gpio != 441U) return 67;
    unsigned chain_index = 99;
    if (!hs_aml_presence_chain_for_gpio(gpio, &chain_index) || chain_index != 2U) return 68;
    struct hs_aml_observation observed = hs_aml_observe_presence(false, NULL, NULL);
    if (observed.read_calls != 0 || observed.bound_profile) return 69;
    for (unsigned i = 0; i < 3U; ++i)
        if (observed.chains[i].plug != HS_AML_PRESENCE_UNKNOWN || observed.chains[i].gpio != 439U + i) return 70;
    /* Attributed Mujina accepted-share fixture: common nonce/version/header
     * fields only, not BM1370/BM1362 board compatibility. See capture dossier. */
    const uint8_t captured[11] = {0xaa,0x55,0x4c,0x03,0x52,0x75,0x0c,0xd2,0x05,0xa2,0x9c};
    rx = hs_bm1362_rx_decode_long(captured, 11);
    if (rx.status != HS_BM1362_RX_DECODED_UNVERIFIED || rx.nonce != UINT32_C(0x7552034c) ||
        rx.internal_nonce != UINT32_C(0x4c035275) || rx.version_bits != UINT32_C(0x00b44000)) return 71;
    uint8_t captured_header[80], expected_hash[32];
    fixture_hex("0040b420fd55646bc162b96dfcd4f201a3f4670d1d3996bc965201000000000000000000"
                "06ddf5f08c36414b95ea54db71a0c28761a98bcf6355919e044f88725519a7cb"
                "d7685468043a02174c035275", captured_header, 80);
    fixture_hex("fe27887d7a685806d8516ce9da43ea948638f7bc97e9accf0437020000000000", expected_hash, 32);
    fixture_hex("000000000000000000000000000000000000000000000000f8ff070000000000", target, 32);
    if (hs_work_check_nonce(captured_header, 80, rx.nonce, target, hash, &meets) != HS_WORK_OK || !meets) return 72;
    for (unsigned i = 0; i < 32; ++i) if (hash[i] != expected_hash[i]) return 73;
    if (hs_work_check_nonce(captured_header, 80, rx.internal_nonce, target, hash, &meets) != HS_WORK_OK || meets) return 74;
    for (unsigned i = 0; i < 80; ++i) snapshot.header[i] = captured_header[i];
    snapshot.header[0] = 0; snapshot.header[1] = 0; snapshot.header[2] = 0; snapshot.header[3] = 0x20;
    for (unsigned i = 0; i < 32; ++i) snapshot.share_target_le[i] = target[i];
    snapshot.version_mask = HS_BM1362_RX_VERSION_BITS_MASK;
    if (hs_job_cache_init(&cache, snapshot.session_tag) != HS_JOB_OK ||
        hs_job_cache_publish(&cache, rx.slot, &snapshot) != HS_JOB_OK ||
        hs_job_cache_check(&cache, &rx, 1050, &share) != HS_JOB_OK ||
        share.nonce != UINT32_C(0x7552034c) || share.full_version != UINT32_C(0x20b44000)) return 75;
    for (unsigned i = 0; i < 32; ++i) if (share.digest[i] != expected_hash[i]) return 76;
    return 0;
}
