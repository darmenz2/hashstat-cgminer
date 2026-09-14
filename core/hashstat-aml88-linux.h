/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HASHSTAT_AML88_LINUX_H
#define HASHSTAT_AML88_LINUX_H
#include "hs_aml88_start.h"
#include "hs_aml_uart.h"
#include <jansson.h>

struct hs88_linux_config {
    unsigned chain, frequency_mhz, chip_id_offset;
    char model[9], gpiochip[128], sensor_bus[128], psu_bus[128];
    char pwm[2][160], trace_path[256];
    unsigned psu_line, reset_line, presence_line, tach_lines[4];
    unsigned sensor_addr[2], eeprom_addr, psu_dac, pwm_period_ns;
    unsigned min_fan_rpm, work_interval_ms;
    double voltage_min, voltage_max, max_temp_c;
    uint8_t eeprom_sha256[32];
    bool pwm_inversed;
};
bool hs88_config_parse(json_t *, struct hs88_linux_config *, char *, size_t);
bool hs88_config_load(const char *, struct hs88_linux_config *, char *, size_t);
struct hs88_linux;
struct hs88_linux *hs88_linux_new(const struct hs88_linux_config *);
void hs88_linux_free(struct hs88_linux *);
const struct hs88_start_ops *hs88_linux_start_ops(void);
struct hs_aml_uart_session *hs88_linux_uart(struct hs88_linux *);
bool hs88_linux_alive(struct hs88_linux *);
void hs88_linux_progress(struct hs88_linux *);
int hs88_linux_read(struct hs88_linux *, uint8_t *, size_t, unsigned timeout_ms);
bool hs88_linux_shutdown(struct hs88_linux *);
const char *hs88_linux_error(struct hs88_linux *);
#endif
