/* SPDX-License-Identifier: GPL-3.0-or-later
 * HashStat RX integrity adapter; public protocol reference: Mujina,
 * Ryan Kuester and contributors. See RX-INTEGRITY.md for pinned attribution.
 */
#ifndef HS_BM1362_INTEGRITY_H
#define HS_BM1362_INTEGRITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HS_BM1362_INTEGRITY_FRAME_SIZE ((size_t)11)

enum hs_bm1362_integrity_status {
    HS_BM1362_INTEGRITY_ARGUMENT = 0,
    HS_BM1362_INTEGRITY_LENGTH,
    HS_BM1362_INTEGRITY_SYNC,
    HS_BM1362_INTEGRITY_CRC_MISMATCH,
    HS_BM1362_INTEGRITY_UNSUPPORTED_TYPE,
    HS_BM1362_INTEGRITY_OK
};
enum hs_bm1362_response_kind {
    HS_BM1362_RESPONSE_UNKNOWN = 0,
    HS_BM1362_RESPONSE_REGISTER,
    HS_BM1362_RESPONSE_NONCE
};
struct hs_bm1362_integrity {
    enum hs_bm1362_integrity_status status;
    enum hs_bm1362_response_kind kind;
    bool framing_valid;
    bool crc_checked;
    bool crc_valid;
    bool supported_type;


    uint8_t crc_residual;
    uint8_t type_bits;
};















struct hs_bm1362_integrity hs_bm1362_inspect_integrity(const uint8_t *frame,
                                                    size_t length);
#endif
