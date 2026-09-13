/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_rx.h"
#include <stdio.h>
#include <string.h>

static unsigned long checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static const uint8_t sample[11] = {
    0xaa, 0x55, 0x01, 0x02, 0x03, 0x04, 0x7f, 0x18, 0x12, 0x34, 0x80
};
static const uint8_t high[11] = {
    0xaa, 0x55, 0xff, 0x5f, 0x12, 0x34, 0x00, 0xff, 0xff, 0xff, 0xff
};

static int zero_fields(struct hs_bm1362_rx_result value)
{
    return value.nonce == 0 && value.internal_nonce == 0 && value.version_bits == 0
        && value.internal_version_bits == 0 && value.slot == 0
        && value.chip_index == 0 && value.core_index == 0
        && value.opaque_payload4 == 0 && value.opaque_tail_low5 == 0;
}

static uint32_t bit_position_oracle(unsigned first, unsigned second, int canonical)
{
    static const unsigned to[16] = {
        13, 14, 15, 0, 1, 2, 3, 4, 21, 22, 23, 8, 9, 10, 11, 12
    };
    static const unsigned canonical_to[16] = {
        21, 22, 23, 24, 25, 26, 27, 28, 13, 14, 15, 16, 17, 18, 19, 20
    };
    unsigned bit, combined = first | (second << 8);
    uint32_t out = 0;
    for (bit = 0; bit < 16; ++bit)
        if ((combined & (1U << bit)) != 0)
            out |= UINT32_C(1) << (canonical ? canonical_to[bit] : to[bit]);
    return out;
}

int main(void)
{
    uint8_t frame[12], before[12];
    struct hs_bm1362_rx_result out;
    struct hs_bm1362_rx_stream stream = {0}, saved;
    unsigned a, b, bit, index, cut;
    size_t length;

    memcpy(frame, sample, 11); frame[11] = 0xf1;
    memcpy(before, frame, sizeof(frame));
    out = hs_bm1362_rx_decode_long(frame, 11);
    CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED);
    CHECK(out.nonce == UINT32_C(0x04030201) && out.internal_nonce == UINT32_C(0x01020304));
    CHECK(out.internal_version_bits == UINT32_C(0x00804602));
    CHECK(out.version_bits == UINT32_C(0x02468000));
    CHECK(out.slot == 3 && out.chip_index == 64 && out.core_index == 0);
    CHECK(out.opaque_payload4 == 0x7f && out.opaque_tail_low5 == 0);
    CHECK(memcmp(frame, before, sizeof(frame)) == 0);
    out = hs_bm1362_rx_decode_long(high, sizeof(high));
    CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED);
    CHECK(out.nonce == UINT32_C(0x34125fff) && out.internal_nonce == UINT32_C(0xff5f1234));
    CHECK(out.internal_version_bits == UINT32_C(0x00e0ff1f));
    CHECK(out.version_bits == UINT32_C(0x1fffe000));
    CHECK(out.slot == 31 && out.chip_index == 87 && out.core_index == 127);
    CHECK(out.opaque_tail_low5 == 31);

    out = hs_bm1362_rx_decode_long(NULL, 11);
    CHECK(out.status == HS_BM1362_RX_INVALID_ARGUMENT && zero_fields(out));
    for (length = 0; length <= 12; ++length) {
        if (length == 11) continue;
        out = hs_bm1362_rx_decode_long(frame, length);
        CHECK(out.status == HS_BM1362_RX_INVALID_LENGTH && zero_fields(out));
    }
    out = hs_bm1362_rx_decode_long(frame, SIZE_MAX);
    CHECK(out.status == HS_BM1362_RX_INVALID_LENGTH && zero_fields(out));
    out = hs_bm1362_rx_decode_long((const uint8_t *)(UINTPTR_MAX - 4), 11);
    CHECK(out.status == HS_BM1362_RX_ADDRESS_OVERFLOW && zero_fields(out));
    for (index = 0; index < 2; ++index) {
        for (bit = 0; bit < 8; ++bit) {
            memcpy(frame, sample, 11); frame[index] ^= (uint8_t)(1U << bit);
            out = hs_bm1362_rx_decode_long(frame, 11);
            CHECK(out.status == HS_BM1362_RX_INVALID_SYNC && zero_fields(out));
        }
    }
    memcpy(frame, sample, 11);
    for (a = 0; a < 128; ++a) {
        frame[10] = (uint8_t)a;
        out = hs_bm1362_rx_decode_long(frame, 11);
        CHECK(out.status == HS_BM1362_RX_NOT_NONCE && zero_fields(out));
    }

    for (a = 128; a < 256; ++a) {
        frame[10] = (uint8_t)a;
        out = hs_bm1362_rx_decode_long(frame, 11);
        CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED);
        CHECK(out.opaque_tail_low5 == (a % 32));
    }
    memcpy(frame, sample, 11);
    for (a = 0; a < 256; ++a) {
        uint32_t nonce = ((uint32_t)a << 17) | UINT32_C(0xfe000123);
        frame[2] = (uint8_t)(nonce >> 24); frame[3] = (uint8_t)(nonce >> 16);
        frame[4] = (uint8_t)(nonce >> 8); frame[5] = (uint8_t)nonce;
        out = hs_bm1362_rx_decode_long(frame, 11);
        if (a < 176) {
            CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED);
            CHECK(out.chip_index == a / 2 && out.core_index == 127);
            CHECK(out.internal_nonce == nonce);
            for (unsigned byte = 0; byte < 4; ++byte)
                CHECK((uint8_t)(out.nonce >> (8 * byte)) == frame[2 + byte]);
        } else {
            CHECK(out.status == HS_BM1362_RX_INVALID_CHIP && zero_fields(out));
        }
    }
    memcpy(frame, sample, 11);
    for (a = 0; a < 256; ++a) {
        frame[7] = (uint8_t)a;
        out = hs_bm1362_rx_decode_long(frame, 11);
        CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED && out.slot == a / 8);
    }
    for (a = 0; a < 256; ++a) {
        for (b = 0; b < 256; ++b) {
            frame[8] = (uint8_t)a; frame[9] = (uint8_t)b;
            out = hs_bm1362_rx_decode_long(frame, 11);
            CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED);
            CHECK(out.internal_version_bits == bit_position_oracle(a, b, 0));
            CHECK(out.version_bits == bit_position_oracle(a, b, 1));
        }
    }

    out = hs_bm1362_rx_stream_push(NULL, 0xaa);
    CHECK(out.status == HS_BM1362_RX_INVALID_ARGUMENT && zero_fields(out));
    out = hs_bm1362_rx_stream_push(
        (struct hs_bm1362_rx_stream *)(UINTPTR_MAX - 4), 0xaa);
    CHECK(out.status == HS_BM1362_RX_ADDRESS_OVERFLOW && zero_fields(out));
    for (a = 11; a < 256; ++a) {
        memset(&stream, 0, sizeof(stream)); stream.used = (uint8_t)a; saved = stream;
        out = hs_bm1362_rx_stream_push(&stream, 0xaa);
        CHECK(out.status == HS_BM1362_RX_INVALID_STATE && zero_fields(out));
        CHECK(memcmp(&stream, &saved, sizeof(stream)) == 0);
    }
    memset(&stream, 0, sizeof(stream)); stream.used = 1; saved = stream;
    out = hs_bm1362_rx_stream_push(&stream, 0xaa);
    CHECK(out.status == HS_BM1362_RX_INVALID_STATE && zero_fields(out));
    CHECK(memcmp(&stream, &saved, sizeof(stream)) == 0);
    stream.used = 2; stream.frame[0] = 0xaa; saved = stream;
    out = hs_bm1362_rx_stream_push(&stream, 0xaa);
    CHECK(out.status == HS_BM1362_RX_INVALID_STATE && zero_fields(out));
    CHECK(memcmp(&stream, &saved, sizeof(stream)) == 0);

    for (cut = 0; cut <= 11; ++cut) {
        memset(&stream, 0, sizeof(stream));
        for (index = 0; index < cut; ++index)
            out = hs_bm1362_rx_stream_push(&stream, sample[index]);
        saved = stream;
        CHECK(stream.used == (cut == 11 ? 0 : cut));
        CHECK(memcmp(&stream, &saved, sizeof(stream)) == 0);
        for (index = cut; index < 11; ++index) {
            out = hs_bm1362_rx_stream_push(&stream, sample[index]);
            if (index < 10)
                CHECK(out.status == HS_BM1362_RX_NEED_MORE && zero_fields(out));
        }
        CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED);
        CHECK(out.nonce == UINT32_C(0x04030201) && stream.used == 0);
        for (index = 0; index < 11; ++index)
            out = hs_bm1362_rx_stream_push(&stream, high[index]);
        CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED && out.chip_index == 87);
        CHECK(stream.used == 0);
    }
    memset(&stream, 0, sizeof(stream));
    for (a = 0; a < 256; ++a) {
        out = hs_bm1362_rx_stream_push(&stream, (uint8_t)a);
        CHECK(out.status == HS_BM1362_RX_NEED_MORE && zero_fields(out));
    }
    out = hs_bm1362_rx_stream_push(&stream, 0xaa);
    CHECK(out.status == HS_BM1362_RX_NEED_MORE && stream.used == 1);
    for (index = 0; index < 11; ++index)
        out = hs_bm1362_rx_stream_push(&stream, sample[index]);
    CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED && stream.used == 0);

    { static const uint8_t noise[9] = {0xaa,0x55,0xff,0xff,0,0,0,0,0};
      for (index = 0; index < 9; ++index)
          out = hs_bm1362_rx_stream_push(&stream, noise[index]);
      out = hs_bm1362_rx_stream_push(&stream, 0xaa);
      out = hs_bm1362_rx_stream_push(&stream, 0x55);

      CHECK(out.status == HS_BM1362_RX_NOT_NONCE && stream.used == 0);
    }
    { static const uint8_t bad[11] = {
        0xaa,0x55,0xff,0xff,0,0,0xaa,0x55,0x01,0x02,0x83
      };
      for (index = 0; index < 11; ++index)
          out = hs_bm1362_rx_stream_push(&stream, bad[index]);
      CHECK(out.status == HS_BM1362_RX_INVALID_CHIP && stream.used == 5);
      CHECK(memcmp(stream.frame, bad + 6, 5) == 0);

      { static const uint8_t rest[6] = {0x04,0x7f,0x18,0x12,0x34,0x80};
        for (index = 0; index < 6; ++index)
            out = hs_bm1362_rx_stream_push(&stream, rest[index]);
        CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED);
        CHECK(out.nonce == UINT32_C(0x04830201) && stream.used == 0);
      }
    }
    memcpy(frame, high, 11); frame[3] = 0xff; frame[10] = 0xaa;
    for (index = 0; index < 11; ++index)
        out = hs_bm1362_rx_stream_push(&stream, frame[index]);
    CHECK(out.status == HS_BM1362_RX_INVALID_CHIP && stream.used == 1);
    for (index = 1; index < 11; ++index)
        out = hs_bm1362_rx_stream_push(&stream, sample[index]);
    CHECK(out.status == HS_BM1362_RX_DECODED_UNVERIFIED && stream.used == 0);

    printf("PASS BM1362 decoded-unverified RX: %lu checks; no hardware or CRC claim\n", checks);
    return 0;
}
