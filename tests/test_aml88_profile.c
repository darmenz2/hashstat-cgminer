/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml88_profile.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks;
#define C(x) do { ++checks; assert(x); } while (0)
static const char *const models[] = {"42801", "BHB42801", "BHB42821", "BHB42831"};
static const uint32_t constructors[] = {0x2c5c0, 0x2dfbc, 0x2f884, 0x3114c};

static const uint32_t topology_words[44] = {
    0x150014,0x170016,0x410040,0x430042, 0x120013,0x180019,0x3e003f,0x440045,
    0x110010,0x1b001a,0x3d003c,0x470046, 0x0e000f,0x1c001d,0x3a003b,0x480049,
    0x0d000c,0x1f001e,0x390038,0x4b004a, 0x0a000b,0x200021,0x360037,0x4c004d,
    0x090008,0x230022,0x350034,0x4f004e, 0x060007,0x240025,0x320033,0x500051,
    0x050004,0x270026,0x310030,0x530052, 0x020003,0x280029,0x2e002f,0x540055,
    0x010000,0x2b002a,0x2d002c,0x570056
};
static void failure(const char *model, size_t m, const char *controller, size_t c,
                    enum hs_aml88_profile_status expected)
{
    struct hs_aml88_profile_result r = hs_aml88_profile_select(model, m, controller, c);
    C(r.status == expected);
    C(r.profile == NULL);
}
static void geometry_checks(const struct hs_aml88_profile *p, unsigned variant)
{
    const struct hs_aml88_geometry *g = p->geometry;
    const uint8_t sensor_positions[] = {20,67,87,0};
    unsigned seen[88] = {0};
    C(p->variant == (enum hs_aml88_variant)(variant + 1U));
    C(p->constructor_va == constructors[variant]);
    C(p->controller == HS_AML88_CONTROLLER_AML);
    C(strcmp(p->btm_model, models[variant]) == 0 && strcmp(p->model, "s19-88") == 0);
    C(g->chip_model == HS_AML88_CHIP_BM1362 && g->chip_core_count == 514);
    C(g->chip_count == 88 && g->domain_count == 44 && g->chips_per_domain == 2);
    C(g->chip_count == g->domain_count * g->chips_per_domain);
    C(g->chain_count == 3 && g->address_step == 2 && g->last_address == 0xae);
    C(g->topology_rows == 11 && g->topology_columns == 8 && g->sensor_count == 4);
    for (unsigned i = 0; i < 4; ++i) {
        C(g->sensors[i].i2c_address == 0x48U + i);
        C(g->sensors[i].chip_position == sensor_positions[i]);
        C(g->sensors[i].access == HS_AML88_SENSOR_VIA_PIC);
        C(g->sensors[i].location == HS_AML88_SENSOR_BACK);
    }
    for (unsigned row = 0; row < 11; ++row) {
        for (unsigned col = 0; col < 8; ++col) {
            unsigned expected = (topology_words[row * 4U + col / 2U] >> ((col % 2U) * 16U)) & 0xffffU;
            uint8_t index = 255;
            struct hs_aml88_position pos = {255,255};
            C(expected < 88);
            C(g->topology[row][col] == expected);
            C(hs_aml88_profile_topology_index(p, row, col, &index) == HS_AML88_PROFILE_OK);
            C(index == expected);
            C(hs_aml88_profile_topology_position(p, index, &pos) == HS_AML88_PROFILE_OK);
            C(pos.row == row && pos.column == col);
            ++seen[index];
        }
    }
    for (unsigned i = 0; i < 88; ++i) {
        uint8_t address = 255, index = 255;
        C(seen[i] == 1);
        C(hs_aml88_profile_chip_address(p, i, &address) == HS_AML88_PROFILE_OK);
        C(address == i * 2U);
        C(hs_aml88_profile_chip_index(p, address, &index) == HS_AML88_PROFILE_OK);
        C(index == i);
    }
    for (unsigned a = 0; a < 512; ++a) {
        uint8_t index = 255;
        bool valid = a <= 174 && a % 2U == 0;
        C(hs_aml88_profile_chip_index(p, a, &index) ==
          (valid ? HS_AML88_PROFILE_OK : HS_AML88_PROFILE_OUT_OF_RANGE));
        C(index == (valid ? a / 2U : 255));
    }
}
static void selector_checks(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        size_t n = strlen(models[i]);
        struct hs_aml88_profile_result r = hs_aml88_profile_select(models[i], n, "aml", 3);
        C(r.status == HS_AML88_PROFILE_OK && r.profile != NULL);
        C(hs_aml88_profile_is_known(r.profile));
        geometry_checks(r.profile, i);

        for (size_t at = 0; at < n; ++at) {
            for (unsigned byte = 0; byte <= 255; ++byte) {
                char input[8];
                int expected = -1;
                memcpy(input, models[i], n);
                input[at] = (char)(unsigned char)byte;
                for (unsigned j = 0; j < 4; ++j)
                    if (strlen(models[j]) == n && memcmp(input, models[j], n) == 0) expected = (int)j;
                r = hs_aml88_profile_select(input, n, "aml", 3);
                C(r.status == (expected < 0 ? HS_AML88_PROFILE_UNSUPPORTED_MODEL : HS_AML88_PROFILE_OK));
                if (expected < 0) C(r.profile == NULL);
                else C(r.profile->variant == (enum hs_aml88_variant)(expected + 1));
            }
        }
        for (size_t nread = 0; nread <= 12; ++nread) {
            char input[12] = {0};
            memcpy(input, models[i], n);
            r = hs_aml88_profile_select(input, nread, "aml", 3);
            C(r.status == (nread == 0 ? HS_AML88_PROFILE_AMBIGUOUS_MODEL :
                           nread == n ? HS_AML88_PROFILE_OK : HS_AML88_PROFILE_UNSUPPORTED_MODEL));
            C((r.profile != NULL) == (nread == n));
        }
    }
    for (unsigned at = 0; at < 3; ++at) {
        for (unsigned byte = 0; byte <= 255; ++byte) {
            char controller[3] = {'a','m','l'};
            controller[at] = (char)(unsigned char)byte;
            struct hs_aml88_profile_result r = hs_aml88_profile_select("42801",5,controller,3);
            bool accepted = memcmp(controller, "aml", 3) == 0;
            C(r.status == (accepted ? HS_AML88_PROFILE_OK : HS_AML88_PROFILE_UNSUPPORTED_CONTROLLER));
            C((r.profile != NULL) == accepted);
        }
    }
    const char *const wrong_models[] = {"s19-126", "BM1398", "BHB42XXX", "S19-88", " 42801", "42801\n", "42801 ", "BHB42811"};
    const char *const wrong_controllers[] = {"stm", "xil", "bb", "cv", "AML", "am", "aml\n", "aml ", "aml,stm"};
    for (size_t i = 0; i < sizeof(wrong_models) / sizeof(wrong_models[0]); ++i)
        failure(wrong_models[i], strlen(wrong_models[i]), "aml", 3, HS_AML88_PROFILE_UNSUPPORTED_MODEL);
    for (size_t i = 0; i < sizeof(wrong_controllers) / sizeof(wrong_controllers[0]); ++i)
        failure("42801", 5, wrong_controllers[i], strlen(wrong_controllers[i]), HS_AML88_PROFILE_UNSUPPORTED_CONTROLLER);
    failure("s19-88",6,"aml",3,HS_AML88_PROFILE_AMBIGUOUS_MODEL);
    failure(NULL,0,"aml",3,HS_AML88_PROFILE_AMBIGUOUS_MODEL);
    failure("",0,"aml",3,HS_AML88_PROFILE_AMBIGUOUS_MODEL);
    failure("42801",5,NULL,0,HS_AML88_PROFILE_AMBIGUOUS_CONTROLLER);
    failure(NULL,0,NULL,0,HS_AML88_PROFILE_AMBIGUOUS_CONTROLLER);
    failure(NULL,1,"aml",3,HS_AML88_PROFILE_ARGUMENT);
    failure("42801",5,NULL,1,HS_AML88_PROFILE_ARGUMENT);
    failure("42801",6,"aml",3,HS_AML88_PROFILE_UNSUPPORTED_MODEL);
    failure("42801",5,"aml",4,HS_AML88_PROFILE_UNSUPPORTED_CONTROLLER);
    failure((const char *)(UINTPTR_MAX - 1U),3,"aml",3,HS_AML88_PROFILE_ARGUMENT);
    failure("42801",5,(const char *)(UINTPTR_MAX - 1U),3,HS_AML88_PROFILE_ARGUMENT);
    failure("42801",SIZE_MAX,"aml",3,HS_AML88_PROFILE_ARGUMENT);
}
static void invalid_helpers(void)
{
    const struct hs_aml88_profile *p = hs_aml88_profile_select("42801",5,"aml",3).profile;
    struct hs_aml88_profile copy = *p;
    uint8_t out = 201;
    struct hs_aml88_position pos = {201,202};
    C(!hs_aml88_profile_is_known(NULL));
    C(!hs_aml88_profile_is_known(&copy));
    const struct hs_aml88_profile *const bad[] = {NULL,&copy};
    for (unsigned i = 0; i < 2; ++i) {
        C(hs_aml88_profile_chip_address(bad[i],0,&out) == HS_AML88_PROFILE_ARGUMENT && out == 201);
        C(hs_aml88_profile_chip_index(bad[i],0,&out) == HS_AML88_PROFILE_ARGUMENT && out == 201);
        C(hs_aml88_profile_topology_index(bad[i],0,0,&out) == HS_AML88_PROFILE_ARGUMENT && out == 201);
        C(hs_aml88_profile_topology_position(bad[i],0,&pos) == HS_AML88_PROFILE_ARGUMENT);
        C(pos.row == 201 && pos.column == 202);
    }
    C(hs_aml88_profile_chip_address(p,0,NULL) == HS_AML88_PROFILE_ARGUMENT);
    C(hs_aml88_profile_chip_index(p,0,NULL) == HS_AML88_PROFILE_ARGUMENT);
    C(hs_aml88_profile_topology_index(p,0,0,NULL) == HS_AML88_PROFILE_ARGUMENT);
    C(hs_aml88_profile_topology_position(p,0,NULL) == HS_AML88_PROFILE_ARGUMENT);
    const unsigned bad_indices[] = {88,126,UINT_MAX};
    for (unsigned i = 0; i < 3; ++i) {
        C(hs_aml88_profile_chip_address(p,bad_indices[i],&out) == HS_AML88_PROFILE_OUT_OF_RANGE && out == 201);
        C(hs_aml88_profile_topology_position(p,bad_indices[i],&pos) == HS_AML88_PROFILE_OUT_OF_RANGE);
        C(pos.row == 201 && pos.column == 202);
    }
    C(hs_aml88_profile_chip_index(p,UINT_MAX,&out) == HS_AML88_PROFILE_OUT_OF_RANGE && out == 201);
    C(hs_aml88_profile_topology_index(p,11,0,&out) == HS_AML88_PROFILE_OUT_OF_RANGE && out == 201);
    C(hs_aml88_profile_topology_index(p,0,8,&out) == HS_AML88_PROFILE_OUT_OF_RANGE && out == 201);
    C(hs_aml88_profile_topology_index(p,UINT_MAX,UINT_MAX,&out) == HS_AML88_PROFILE_OUT_OF_RANGE && out == 201);
    C(hs_aml88_profile_topology_position(p,0,(struct hs_aml88_position *)UINTPTR_MAX) == HS_AML88_PROFILE_ARGUMENT);
}
int main(void)
{
    selector_checks();
    invalid_helpers();
    printf("AML88 profile: %lu checks passed\n", checks);
    return 0;
}
