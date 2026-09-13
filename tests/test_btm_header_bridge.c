/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_btm_work_wire.h"
#include <stdio.h>
#include <string.h>

static unsigned long checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; \
} } while (0)

static unsigned digit(char c)
{
    return (unsigned)(c >= 'a' ? c - 'a' + 10 : c - '0');
}
static void hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = (uint8_t)(digit(s[i * 2]) * 16U + digit(s[i * 2 + 1]));
}
static int filled(const uint8_t *bytes, size_t n, uint8_t value)
{
    for (size_t i = 0; i < n; ++i) if (bytes[i] != value) return 0;
    return 1;
}

static void producer_oracle(const uint8_t header[80], uint8_t ring[76])
{
    uint8_t intermediate[76];
    for (size_t i = 0; i < 76; ++i)
        intermediate[i] = header[(i & ~(size_t)3) | (3U - (i & 3U))];
    for (size_t i = 0; i < 64; ++i) ring[i] = intermediate[63U - i];
    for (size_t i = 0; i < 12; ++i) ring[64U + i] = intermediate[75U - i];
}

int main(void)
{
    static const char genesis_header_hex[] =
        "010000000000000000000000000000000000000000000000000000000000000000000000"
        "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
        "29ab5f49ffff001d1dac2b7c";

    static const char genesis_ring_hex[] =
        "3a9fb8aa888a51327fc81bc367768f617ac72c3e7a7b12b23ba3edfd"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "01000000ffff001d29ab5f494b1e5e4a";
    static const char genesis_packet_hex[] =
        "55aa2136280100000000ffff001d29ab5f494b1e5e4a"
        "3a9fb8aa888a51327fc81bc367768f617ac72c3e7a7b12b23ba3edfd"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "01000000bb38";
    uint8_t header[80], saved_header[80], ring[96], expected[76], packet[88];
    uint8_t expected_packet[88], arena[240], saved_arena[240];
    struct hs_work_wire_result out;
    unsigned value;
    size_t i, input, output;

    CHECK(strlen(genesis_header_hex) == 160);
    CHECK(strlen(genesis_ring_hex) == 152);
    CHECK(strlen(genesis_packet_hex) == 176);
    hex(genesis_header_hex, header, 80);
    hex(genesis_ring_hex, expected, 76);
    hex(genesis_packet_hex, expected_packet, 88);
    memcpy(saved_header, header, 80); memset(ring, 0xa5, sizeof(ring));
    out = hs_btm_work_ring_from_header80(header, 80, ring, sizeof(ring));
    CHECK(out.status == HS_WORK_WIRE_OK && out.written == 76);
    CHECK(memcmp(ring, expected, 76) == 0);
    CHECK(filled(ring + 76, sizeof(ring) - 76, 0xa5));
    CHECK(memcmp(header, saved_header, 80) == 0);
    out = hs_btm_work_wire_pack(ring, 76, 5, packet, sizeof(packet));
    CHECK(out.status == HS_WORK_WIRE_OK && out.written == 88);
    CHECK(memcmp(packet, expected_packet, 88) == 0);

    for (input = 0; input < 80; ++input) {
        for (value = 0; value < 256; ++value) {
            for (i = 0; i < 80; ++i) header[i] = (uint8_t)(i * 73U + 19U);
            header[input] = (uint8_t)value;
            producer_oracle(header, expected);
            memset(ring, 0xa5, sizeof(ring)); memcpy(saved_header, header, 80);
            out = hs_btm_work_ring_from_header80(header, 80, ring, sizeof(ring));
            CHECK(out.status == HS_WORK_WIRE_OK && out.written == 76);
            CHECK(memcmp(ring, expected, 76) == 0);
            CHECK(filled(ring + 76, sizeof(ring) - 76, 0xa5));
            CHECK(memcmp(header, saved_header, 80) == 0);
            out = hs_btm_work_wire_pack(ring, 76, value % 32U, packet, 88);
            CHECK(out.status == HS_WORK_WIRE_OK && out.written == 88);
            for (i = 0; i < 76; ++i)
                CHECK(packet[10U + i] == header[(18U - i / 4U) * 4U + i % 4U]);
        }
    }

#define FAIL(call, expected_status) do { \
    memset(ring, 0xa5, sizeof(ring)); memcpy(saved_header, header, 80); \
    out = (call); CHECK(out.status == (expected_status) && out.written == 0); \
    CHECK(filled(ring, sizeof(ring), 0xa5)); \
    CHECK(memcmp(header, saved_header, 80) == 0); \
} while (0)
    FAIL(hs_btm_work_ring_from_header80(NULL, 80, ring, 76), HS_WORK_WIRE_INVALID_ARGUMENT);
    FAIL(hs_btm_work_ring_from_header80(header, 80, NULL, 76), HS_WORK_WIRE_INVALID_ARGUMENT);
    for (i = 0; i <= 96; ++i) {
        if (i != 80)
            FAIL(hs_btm_work_ring_from_header80(header, i, ring, 76), HS_WORK_WIRE_INVALID_LENGTH);
        if (i < 76)
            FAIL(hs_btm_work_ring_from_header80(header, 80, ring, i), HS_WORK_WIRE_INSUFFICIENT_CAPACITY);
    }
    FAIL(hs_btm_work_ring_from_header80(header, SIZE_MAX, ring, 76), HS_WORK_WIRE_INVALID_LENGTH);
    FAIL(hs_btm_work_ring_from_header80((uint8_t *)(UINTPTR_MAX - 40), 80, ring, 76),
         HS_WORK_WIRE_ADDRESS_OVERFLOW);
    FAIL(hs_btm_work_ring_from_header80(header, 80, (uint8_t *)(UINTPTR_MAX - 40), 76),
         HS_WORK_WIRE_ADDRESS_OVERFLOW);

    for (input = 0; input <= sizeof(arena) - 80; ++input) {
        for (output = 0; output <= sizeof(arena) - 76; ++output) {
            for (i = 0; i < sizeof(arena); ++i) arena[i] = (uint8_t)(i * 11U + 7U);
            memcpy(saved_arena, arena, sizeof(arena));
            producer_oracle(arena + input, expected);
            out = hs_btm_work_ring_from_header80(arena + input, 80, arena + output, 76);
            if (input < output + 76 && output < input + 80) {
                CHECK(out.status == HS_WORK_WIRE_OVERLAP && out.written == 0);
                CHECK(memcmp(arena, saved_arena, sizeof(arena)) == 0);
            } else {
                CHECK(out.status == HS_WORK_WIRE_OK && out.written == 76);
                memcpy(saved_arena + output, expected, 76);
                CHECK(memcmp(arena, saved_arena, sizeof(arena)) == 0);
            }
        }
    }
    printf("PASS canonical header -> ring -> work packet: %lu checks\n", checks);
    return 0;
}
