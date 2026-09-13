/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original C adapter of the public BM13xx CRC protocol described by Mujina,
 * Ryan Kuester and contributors; pinned sources/fixtures: RX-INTEGRITY.md.
 */
#include "hs_bm1362_integrity.h"

struct hs_bm1362_integrity hs_bm1362_inspect_integrity(const uint8_t *frame,
                                                    size_t length)
{
    struct hs_bm1362_integrity result = {0};
    unsigned crc = 0x1fu;
    size_t i;

    if (frame == NULL) return result;
    if (length != HS_BM1362_INTEGRITY_FRAME_SIZE) {
        result.status = HS_BM1362_INTEGRITY_LENGTH;
        return result;
    }
    if ((uintptr_t)frame > UINTPTR_MAX - (HS_BM1362_INTEGRITY_FRAME_SIZE - 1u))
        return result;
    if (frame[0] != 0xaau || frame[1] != 0x55u) {
        result.status = HS_BM1362_INTEGRITY_SYNC;
        return result;
    }
    result.framing_valid = true;
    for (i = 2; i < HS_BM1362_INTEGRITY_FRAME_SIZE; ++i) {
        unsigned shift;
        for (shift = 8; shift > 0; --shift) {
            unsigned input_bit = ((unsigned)frame[i] >> (shift - 1u)) & 1u;
            unsigned feedback = ((crc >> 4u) & 1u) ^ input_bit;
            crc = ((crc << 1u) & 0x1fu) ^ (feedback ? 0x05u : 0u);
        }
    }
    result.crc_checked = true;
    result.crc_residual = (uint8_t)crc;
    result.crc_valid = crc == 0u;
    result.type_bits = (uint8_t)(frame[10] & 0xe0u);
    switch (result.type_bits) {
    case 0x00:
        result.kind = HS_BM1362_RESPONSE_REGISTER;
        result.supported_type = true;
        break;
    case 0x80:
        result.kind = HS_BM1362_RESPONSE_NONCE;
        result.supported_type = true;
        break;
    default:
        break;
    }
    if (!result.crc_valid) result.status = HS_BM1362_INTEGRITY_CRC_MISMATCH;
    else if (!result.supported_type) result.status = HS_BM1362_INTEGRITY_UNSUPPORTED_TYPE;
    else result.status = HS_BM1362_INTEGRITY_OK;
    return result;
}
