/* SPDX-License-Identifier: GPL-3.0-only */
/* Packet/header fixture data: Mujina, GPL-3.0-or-later,
 * commit 019d1b0916457bb7ecd7cb7e93fee63c0329f9a2, test_data.rs.
 * See CAPTURE-INTEROPERABILITY.md for provenance and model limitations. */
#include "hs_bm1362_rx.h"
#include "hs_job_cache.h"
#include "hs_work.h"
#include "hs_btm_work_wire.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "capture FAIL line %d: %s\n", __LINE__, #c); return 1; \
} } while (0)
static unsigned digit(char c)
{
    return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a') + 10U;
}
static void unhex(const char *s, uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        out[i] = (uint8_t)((digit(s[i * 2]) << 4) | digit(s[i * 2 + 1]));
}

int main(void)
{
    const uint8_t frame[11] = {0xaa,0x55,0x4c,0x03,0x52,0x75,0x0c,0xd2,0x05,0xa2,0x9c};
    uint8_t header[80], expected[32], target[32], hash[32];
    unhex("0040b420fd55646bc162b96dfcd4f201a3f4670d1d3996bc965201000000000000000000"
          "06ddf5f08c36414b95ea54db71a0c28761a98bcf6355919e044f88725519a7cb"
          "d7685468043a02174c035275", header, sizeof(header));
    unhex("fe27887d7a685806d8516ce9da43ea948638f7bc97e9accf0437020000000000", expected, 32);
    unhex("000000000000000000000000000000000000000000000000f8ff070000000000", target, 32);
    struct hs_bm1362_rx_result reply = hs_bm1362_rx_decode_long(frame, sizeof(frame));
    CHECK(reply.status == HS_BM1362_RX_DECODED_UNVERIFIED);
    CHECK(reply.nonce == UINT32_C(0x7552034c));
    CHECK(reply.version_bits == UINT32_C(0x00b44000));
    int meets = -1;
    CHECK(hs_work_check_nonce(header, 80, reply.nonce, target, hash, &meets) == HS_WORK_OK);
    CHECK(meets == 1 && memcmp(hash, expected, 32) == 0);
    CHECK(hs_work_check_nonce(header, 80, UINT32_C(0x4c035275), target, hash, &meets) == HS_WORK_OK);
    CHECK(meets == 0 && memcmp(hash, expected, 32) != 0);

    struct hs_job_cache cache;
    struct hs_job_snapshot job = {0};
    struct hs_checked_share checked;
    memcpy(job.header, header, 80);

    job.header[0] = 0; job.header[1] = 0; job.header[2] = 0; job.header[3] = 0x20;
    memcpy(job.share_target_le, target, 32);
    job.session_tag = 9; job.job_tag = 10; job.work_tag = 11;
    job.issued_ms = 100; job.max_age_ms = 200;
    job.version_mask = HS_BM1362_RX_VERSION_BITS_MASK;
    CHECK(hs_job_cache_init(&cache, 9) == HS_JOB_OK);
    CHECK(hs_job_cache_publish(&cache, reply.slot, &job) == HS_JOB_OK);
    CHECK(hs_job_cache_check(&cache, &reply, 101, &checked) == HS_JOB_OK);
    CHECK(checked.nonce == UINT32_C(0x7552034c) && checked.full_version == UINT32_C(0x20b44000));
    CHECK(memcmp(checked.digest, expected, 32) == 0);
    CHECK(hs_job_cache_check(&cache, &reply, 101, &checked) == HS_JOB_DUPLICATE);

    uint8_t tx[88], encoded[88], base[80] = {0}, prefix[76];
    unhex("55aa2136000100000000267702176e49b66790523e5b37df504ae0a13fc0f2cb93b9"
          "4a6b42224f7521631476b5d6dc20cc270000000000000000dd4100007df8a587"
          "a31dd9ffe1cc3aad8b1f17eefa0284080000002062b9", tx, 88);

    memcpy(base, tx + 82, 4);
    memcpy(base + 68, tx + 14, 4); memcpy(base + 72, tx + 10, 4);
    for (size_t word = 0; word < 8; ++word) {
        memcpy(base + 4 + word * 4, tx + 50 + (7 - word) * 4, 4);
        memcpy(base + 36 + word * 4, tx + 18 + (7 - word) * 4, 4);
    }
    CHECK(hs_btm_work_ring_from_header80(base, 80, prefix, 76).status == HS_WORK_WIRE_OK);
    CHECK(hs_btm_work_wire_pack(prefix, 76, 0, encoded, 88).status == HS_WORK_WIRE_OK);
    CHECK(memcmp(tx, encoded, 88) == 0);
    puts("capture interoperability PASS (no hardware execution)");
    printf("checks=%u\n", checks);
    return 0;
}
