/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML88_START_H
#define HS_AML88_START_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One chain. Electrical ownership is established by prepare(), not by this
 * protocol engine. Every callback must be bounded by timeout_ms. */
struct hs88_start_ops {
    bool (*prepare)(void *);
    bool (*reset)(void *, bool asserted);
    bool (*healthy)(void *);
    bool (*cancelled)(void *);
    bool (*now)(void *, uint64_t *);
    bool (*sleep)(void *, unsigned milliseconds);
    bool (*flush_rx)(void *);
    bool (*write)(void *, const uint8_t *, size_t, unsigned timeout_ms);
    /* >0 bytes, 0 timeout/no data, -1 transport failure. */
    int (*read)(void *, uint8_t *, size_t, unsigned timeout_ms);
    bool (*off)(void *);
    void (*trace)(void *, const char *stage, unsigned chip,
                  const uint8_t *, size_t, bool receive);
};
struct hs88_start_config {
    unsigned frequency_mhz;      /* Bench range: 50..200 MHz. */
    unsigned timeout_ms;         /* Whole startup, 1000..120000. */
    unsigned reply_timeout_ms;   /* 20..2000. */
    unsigned chip_id_offset;     /* Explicitly select 2 or 3, no auto guess. */
};
enum hs88_start_status {
    HS88_START_OK = 0, HS88_START_ARGUMENT, HS88_START_CANCELLED,
    HS88_START_CLOCK, HS88_START_TIMEOUT, HS88_START_INTERLOCK,
    HS88_START_IO, HS88_START_COUNT, HS88_START_ID,
    HS88_START_READBACK, HS88_START_PROTOCOL
};
struct hs88_start_result {
    enum hs88_start_status status;
    const char *stage;
    unsigned chip, discovered, assigned, configured, crc_errors;
    uint32_t expected, observed, pll;
    uint64_t tx_bytes, rx_bytes;
    bool reset_asserted_on_error, power_off_on_error;
};
struct hs88_start_result hs88_start_chain(const struct hs88_start_config *,
                                         const struct hs88_start_ops *, void *);
const char *hs88_start_status_name(enum hs88_start_status);
#endif
