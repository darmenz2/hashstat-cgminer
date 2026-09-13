/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#include "hs_aml_readonly.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int hs_aml_readonly_diagnostic_main(int argc, char **argv, FILE *output);
static unsigned checks, calls, mode;
#define CHECK(x) do { assert(x); ++checks; } while (0)

struct hs_aml_text hs_aml_readonly_attribute(void *context, unsigned chain,
                                           enum hs_aml_attribute attribute)
{
    CHECK(context == NULL);
    CHECK(chain == calls / 3U);
    CHECK((unsigned)attribute == calls % 3U);
    ++calls;
    struct hs_aml_text result = {0};
    if (mode == 1U) return result;
    if (mode == 2U) {
        result.status = HS_AML_TEXT_READ_ERROR;
        return result;
    }
    result.status = HS_AML_TEXT_OK;
    result.length = 4;
    result.bytes[0] = 0; result.bytes[1] = '\n';
    result.bytes[2] = '"'; result.bytes[3] = 0xff;
    if (mode == 3U) result.length = HS_AML_OBSERVE_TEXT_CAPACITY + 1U;
    if (mode == 4U) result.status = (enum hs_aml_text_status)99;
    if (mode == 5U) result.status = HS_AML_TEXT_READ_ERROR;
    if (mode == 6U) { result.status = HS_AML_TEXT_TOO_LONG; result.length = 0; }
    if (mode == 7U) { result.status = HS_AML_TEXT_UNSAFE_TYPE; result.length = 0; }
    return result;
}

static int run(unsigned selected, int argc, char **argv, char *text, size_t cap)
{
    calls = 0; mode = selected;
    FILE *file = tmpfile(); CHECK(file != NULL);
    const int result = hs_aml_readonly_diagnostic_main(argc, argv, file);
    CHECK(fflush(file) == 0); CHECK(fseek(file, 0, SEEK_SET) == 0);
    const size_t count = fread(text, 1, cap - 1U, file);
    CHECK(!ferror(file)); CHECK(count < cap - 1U);
    text[count] = 0; CHECK(fclose(file) == 0);
    return result;
}

int main(void)
{
    char output[4096];
    char *accepted[] = {"diagnostic", "--raw-readonly-unbound", NULL};
    char *wrong[] = {"diagnostic", "--enable-gpio", NULL};
    char *null_option[] = {"diagnostic", NULL};
    CHECK(run(0, 1, accepted, output, sizeof(output)) == 64 && calls == 0);
    CHECK(run(0, 2, wrong, output, sizeof(output)) == 64 && calls == 0);
    CHECK(run(0, 3, accepted, output, sizeof(output)) == 64 && calls == 0);
    CHECK(run(0, 2, NULL, output, sizeof(output)) == 64 && calls == 0);
    CHECK(run(0, 2, null_option, output, sizeof(output)) == 64 && calls == 0);
    CHECK(hs_aml_readonly_diagnostic_main(2, accepted, NULL) == 74);
    CHECK(run(0, 2, accepted, output, sizeof(output)) == 0 && calls == 9);
    CHECK(strstr(output, "\"profileBound\":true") == NULL);
    CHECK(strstr(output, "\"hashboardReady\":true") == NULL);
    CHECK(strstr(output, "\"rawHex\":\"000a22ff\"") != NULL);
    CHECK(strstr(output, "\"readOK\":9,\"readNotOK\":0") != NULL);
    CHECK(strstr(output, "\"gpio\":439,\"referenceChainIndex\":0") != NULL);
    CHECK(strstr(output, "\"gpio\":441,\"referenceChainIndex\":2") != NULL);
    for (unsigned selected = 1; selected <= 7; ++selected) {
        const int result = run(selected, 2, accepted, output, sizeof(output));
        if (selected >= 3U && selected <= 5U) {
            CHECK(result == 70 && calls == 1);
            CHECK(strstr(output, "reader_contract_error") != NULL);
            CHECK(strstr(output, "raw_report_complete") == NULL);
        } else {
            CHECK(result == 0 && calls == 9);
            CHECK(strstr(output, "\"readOK\":0,\"readNotOK\":9") != NULL);
            CHECK(strstr(output, "\"rawHex\":\"\"") != NULL);
        }
    }

    for (unsigned selected = 0; selected < 3; ++selected) {
        FILE *broken = tmpfile(); CHECK(broken != NULL);
        CHECK(close(fileno(broken)) == 0);
        mode = selected == 2U ? 3U : 0U; calls = 0;
        CHECK(hs_aml_readonly_diagnostic_main(selected == 1U ? 1 : 2,
                                             accepted, broken) == 74);

        CHECK(calls <= (selected == 1U ? 0U : selected == 2U ? 1U : 9U));
        (void)fclose(broken);
    }
    printf("RAW DIAGNOSTIC MOCK PASS: %u assertions; no sysfs reads\n", checks);
    return 0;
}
