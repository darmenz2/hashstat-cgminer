/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml88_profile.h"
#include "hs_span.h"

static const struct hs_aml88_geometry geometry = {
    .chip_model = HS_AML88_CHIP_BM1362, .chip_core_count = 514,
    .chain_count = HS_AML88_CHAIN_COUNT, .chip_count = HS_AML88_CHIP_COUNT,
    .domain_count = HS_AML88_DOMAIN_COUNT, .chips_per_domain = HS_AML88_CHIPS_PER_DOMAIN,
    .address_step = HS_AML88_ADDRESS_STEP, .last_address = HS_AML88_LAST_ADDRESS,
    .topology_rows = HS_AML88_TOPOLOGY_ROWS, .topology_columns = HS_AML88_TOPOLOGY_COLUMNS,
    .sensor_count = HS_AML88_SENSOR_COUNT,
    .sensors = {
        {0x48, 20, HS_AML88_SENSOR_VIA_PIC, HS_AML88_SENSOR_BACK},
        {0x49, 67, HS_AML88_SENSOR_VIA_PIC, HS_AML88_SENSOR_BACK},
        {0x4a, 87, HS_AML88_SENSOR_VIA_PIC, HS_AML88_SENSOR_BACK},
        {0x4b,  0, HS_AML88_SENSOR_VIA_PIC, HS_AML88_SENSOR_BACK}
    },
    .topology = {
        {20,21,22,23,64,65,66,67}, {19,18,25,24,63,62,69,68},
        {16,17,26,27,60,61,70,71}, {15,14,29,28,59,58,73,72},
        {12,13,30,31,56,57,74,75}, {11,10,33,32,55,54,77,76},
        { 8, 9,34,35,52,53,78,79}, { 7, 6,37,36,51,50,81,80},
        { 4, 5,38,39,48,49,82,83}, { 3, 2,41,40,47,46,85,84},
        { 0, 1,42,43,44,45,86,87}
    }
};
static const struct hs_aml88_profile profiles[] = {
    {HS_AML88_VARIANT_42801, HS_AML88_CONTROLLER_AML, "s19-88", "42801", UINT32_C(0x2c5c0), &geometry},
    {HS_AML88_VARIANT_BHB42801, HS_AML88_CONTROLLER_AML, "s19-88", "BHB42801", UINT32_C(0x2dfbc), &geometry},
    {HS_AML88_VARIANT_BHB42821, HS_AML88_CONTROLLER_AML, "s19-88", "BHB42821", UINT32_C(0x2f884), &geometry},
    {HS_AML88_VARIANT_BHB42831, HS_AML88_CONTROLLER_AML, "s19-88", "BHB42831", UINT32_C(0x3114c), &geometry}
};
#define PROFILE_COUNT (sizeof(profiles) / sizeof(profiles[0]))
_Static_assert(HS_AML88_DOMAIN_COUNT * HS_AML88_CHIPS_PER_DOMAIN == HS_AML88_CHIP_COUNT,
               "S19-88 domain count");
_Static_assert(HS_AML88_TOPOLOGY_ROWS * HS_AML88_TOPOLOGY_COLUMNS == HS_AML88_CHIP_COUNT,
               "S19-88 topology count");
_Static_assert((HS_AML88_CHIP_COUNT - 1U) * HS_AML88_ADDRESS_STEP == HS_AML88_LAST_ADDRESS,
               "S19-88 address range");

static bool same(const char *a, size_t n, const char *b, size_t expected)
{
    if (n != expected) return false;
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}
struct hs_aml88_profile_result hs_aml88_profile_select(
    const char *model, size_t model_bytes, const char *controller, size_t controller_bytes)
{
    struct hs_aml88_profile_result r = {HS_AML88_PROFILE_ARGUMENT, NULL};
    if (!hs_span_valid(model, model_bytes) || !hs_span_valid(controller, controller_bytes)) return r;
    r.status = HS_AML88_PROFILE_AMBIGUOUS_CONTROLLER;
    if (controller_bytes == 0) return r;
    r.status = HS_AML88_PROFILE_UNSUPPORTED_CONTROLLER;
    if (!same(controller, controller_bytes, "aml", 3)) return r;
    r.status = HS_AML88_PROFILE_AMBIGUOUS_MODEL;
    if (model_bytes == 0 || same(model, model_bytes, "s19-88", 6)) return r;
    r.status = HS_AML88_PROFILE_UNSUPPORTED_MODEL;
    for (size_t i = 0; i < PROFILE_COUNT; ++i) {
        if (same(model, model_bytes, profiles[i].btm_model, i == 0 ? 5U : 8U)) {
            r.status = HS_AML88_PROFILE_OK;
            r.profile = &profiles[i];
            break;
        }
    }
    return r;
}
bool hs_aml88_profile_is_known(const struct hs_aml88_profile *profile)
{
    for (size_t i = 0; i < PROFILE_COUNT; ++i) if (profile == &profiles[i]) return true;
    return false;
}
enum hs_aml88_profile_status hs_aml88_profile_chip_address(
    const struct hs_aml88_profile *profile, unsigned chip_index, uint8_t *address)
{
    if (!hs_aml88_profile_is_known(profile) || !hs_span_valid(address, sizeof(*address)))
        return HS_AML88_PROFILE_ARGUMENT;
    if (chip_index >= HS_AML88_CHIP_COUNT) return HS_AML88_PROFILE_OUT_OF_RANGE;
    *address = (uint8_t)(chip_index * HS_AML88_ADDRESS_STEP);
    return HS_AML88_PROFILE_OK;
}
enum hs_aml88_profile_status hs_aml88_profile_chip_index(
    const struct hs_aml88_profile *profile, unsigned address, uint8_t *chip_index)
{
    if (!hs_aml88_profile_is_known(profile) || !hs_span_valid(chip_index, sizeof(*chip_index)))
        return HS_AML88_PROFILE_ARGUMENT;
    if (address > HS_AML88_LAST_ADDRESS || address % HS_AML88_ADDRESS_STEP != 0)
        return HS_AML88_PROFILE_OUT_OF_RANGE;
    *chip_index = (uint8_t)(address / HS_AML88_ADDRESS_STEP);
    return HS_AML88_PROFILE_OK;
}
enum hs_aml88_profile_status hs_aml88_profile_topology_index(
    const struct hs_aml88_profile *profile, unsigned row, unsigned column, uint8_t *chip_index)
{
    if (!hs_aml88_profile_is_known(profile) || !hs_span_valid(chip_index, sizeof(*chip_index)))
        return HS_AML88_PROFILE_ARGUMENT;
    if (row >= HS_AML88_TOPOLOGY_ROWS || column >= HS_AML88_TOPOLOGY_COLUMNS)
        return HS_AML88_PROFILE_OUT_OF_RANGE;
    *chip_index = geometry.topology[row][column];
    return HS_AML88_PROFILE_OK;
}
enum hs_aml88_profile_status hs_aml88_profile_topology_position(
    const struct hs_aml88_profile *profile, unsigned chip_index, struct hs_aml88_position *position)
{
    if (!hs_aml88_profile_is_known(profile) || !hs_span_valid(position, sizeof(*position)))
        return HS_AML88_PROFILE_ARGUMENT;
    if (chip_index >= HS_AML88_CHIP_COUNT) return HS_AML88_PROFILE_OUT_OF_RANGE;
    for (unsigned row = 0; row < HS_AML88_TOPOLOGY_ROWS; ++row) {
        for (unsigned column = 0; column < HS_AML88_TOPOLOGY_COLUMNS; ++column) {
            if (geometry.topology[row][column] == chip_index) {
                position->row = (uint8_t)row;
                position->column = (uint8_t)column;
                return HS_AML88_PROFILE_OK;
            }
        }
    }
    return HS_AML88_PROFILE_OUT_OF_RANGE;
}
