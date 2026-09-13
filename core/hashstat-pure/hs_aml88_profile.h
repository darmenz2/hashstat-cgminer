/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML88_PROFILE_H
#define HS_AML88_PROFILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HS_AML88_CHAIN_COUNT 3U
#define HS_AML88_CHIP_COUNT 88U
#define HS_AML88_DOMAIN_COUNT 44U
#define HS_AML88_CHIPS_PER_DOMAIN 2U
#define HS_AML88_ADDRESS_STEP 2U
#define HS_AML88_LAST_ADDRESS 0xaeU
#define HS_AML88_TOPOLOGY_ROWS 11U
#define HS_AML88_TOPOLOGY_COLUMNS 8U
#define HS_AML88_SENSOR_COUNT 4U

enum hs_aml88_variant {
    HS_AML88_VARIANT_UNKNOWN = 0, HS_AML88_VARIANT_42801,
    HS_AML88_VARIANT_BHB42801, HS_AML88_VARIANT_BHB42821,
    HS_AML88_VARIANT_BHB42831
};
enum hs_aml88_controller { HS_AML88_CONTROLLER_UNKNOWN = 0, HS_AML88_CONTROLLER_AML };
enum hs_aml88_chip_model { HS_AML88_CHIP_UNKNOWN = 0, HS_AML88_CHIP_BM1362 };
enum hs_aml88_sensor_access { HS_AML88_SENSOR_ACCESS_UNKNOWN = 0, HS_AML88_SENSOR_VIA_PIC };
enum hs_aml88_sensor_location { HS_AML88_SENSOR_LOCATION_UNKNOWN = 0, HS_AML88_SENSOR_BACK };
enum hs_aml88_profile_status {
    HS_AML88_PROFILE_OK = 0, HS_AML88_PROFILE_ARGUMENT,
    HS_AML88_PROFILE_AMBIGUOUS_MODEL, HS_AML88_PROFILE_UNSUPPORTED_MODEL,
    HS_AML88_PROFILE_AMBIGUOUS_CONTROLLER, HS_AML88_PROFILE_UNSUPPORTED_CONTROLLER,
    HS_AML88_PROFILE_OUT_OF_RANGE
};
struct hs_aml88_sensor {
    uint8_t i2c_address;
    uint8_t chip_position;
    enum hs_aml88_sensor_access access;
    enum hs_aml88_sensor_location location;
};
struct hs_aml88_geometry {
    enum hs_aml88_chip_model chip_model;
    uint16_t chip_core_count;
    uint8_t chain_count, chip_count, domain_count, chips_per_domain;
    uint8_t address_step, last_address, topology_rows, topology_columns, sensor_count;
    struct hs_aml88_sensor sensors[HS_AML88_SENSOR_COUNT];
    uint8_t topology[HS_AML88_TOPOLOGY_ROWS][HS_AML88_TOPOLOGY_COLUMNS];
};
struct hs_aml88_profile {
    enum hs_aml88_variant variant;
    enum hs_aml88_controller controller;
    char model[7];
    char btm_model[9];
    uint32_t constructor_va;
    const struct hs_aml88_geometry *geometry;
};
struct hs_aml88_profile_result {
    enum hs_aml88_profile_status status;
    const struct hs_aml88_profile *profile;
};
struct hs_aml88_position { uint8_t row, column; };

struct hs_aml88_profile_result hs_aml88_profile_select(
    const char *model, size_t model_bytes, const char *controller, size_t controller_bytes);
bool hs_aml88_profile_is_known(const struct hs_aml88_profile *profile);

enum hs_aml88_profile_status hs_aml88_profile_chip_address(
    const struct hs_aml88_profile *profile, unsigned chip_index, uint8_t *address);
enum hs_aml88_profile_status hs_aml88_profile_chip_index(
    const struct hs_aml88_profile *profile, unsigned address, uint8_t *chip_index);
enum hs_aml88_profile_status hs_aml88_profile_topology_index(
    const struct hs_aml88_profile *profile, unsigned row, unsigned column, uint8_t *chip_index);
enum hs_aml88_profile_status hs_aml88_profile_topology_position(
    const struct hs_aml88_profile *profile, unsigned chip_index, struct hs_aml88_position *position);
#endif
