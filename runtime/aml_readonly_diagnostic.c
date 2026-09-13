/* SPDX-License-Identifier: GPL-3.0-only */

#include "hs_aml_readonly.h"
#include <stdio.h>
#include <string.h>

int hs_aml_readonly_diagnostic_main(int argc, char **argv, FILE *output);

int hs_aml_readonly_diagnostic_main(int argc, char **argv, FILE *output)
{
    static const char *const attributes[] = {"direction", "active_low", "value"};
    static const char *const statuses[] = {
        "unavailable", "ok", "read_error", "too_long", "unsafe_type"
    };
    if (output == NULL) return 74;
    if (argc != 2 || argv == NULL || argv[1] == NULL ||
        strcmp(argv[1], "--raw-readonly-unbound") != 0) {
        if (fputs("{\"status\":\"opt_in_required\",\"requiredArgument\":"
                  "\"--raw-readonly-unbound\",\"sysfsReads\":0}\n", output) == EOF ||
            fflush(output) != 0) return 74;
        return 64;
    }
    if (fputs("{\"schema\":\"hashstat.aml.raw-attributes.v1\","
              "\"profileBound\":false,\"modelValidated\":false,"
              "\"hashboardReady\":false,\"atomicSnapshot\":false,"
              "\"diagnosticOnly\":true,\"gpioWrites\":0,"
              "\"uartOpened\":false,\"i2cOpened\":false,"
              "\"firmwareInstalled\":false}\n", output) == EOF) return 74;
    unsigned ok = 0;
    for (unsigned chain = 0; chain < 3U; ++chain) {
        for (unsigned attr = 0; attr < 3U; ++attr) {
            const struct hs_aml_text text = hs_aml_readonly_attribute(
                NULL, chain, (enum hs_aml_attribute)attr);

            if ((unsigned)text.status > HS_AML_TEXT_UNSAFE_TYPE ||
                text.length > HS_AML_OBSERVE_TEXT_CAPACITY ||
                (text.status != HS_AML_TEXT_OK && text.length != 0U)) {
                if (fputs("{\"status\":\"reader_contract_error\","
                          "\"profileBound\":false}\n", output) == EOF ||
                    fflush(output) != 0) return 74;
                return 70;
            }
            if (text.status == HS_AML_TEXT_OK) ++ok;
            if (fprintf(output, "{\"gpio\":%u,\"referenceChainIndex\":%u,"
                        "\"attribute\":\"%s\",\"status\":\"%s\","
                        "\"length\":%zu,\"rawHex\":\"",
                        439U + chain, chain, attributes[attr],
                        statuses[(unsigned)text.status], text.length) < 0) return 74;
            for (size_t i = 0; i < text.length; ++i) {
                if (fprintf(output, "%02x", (unsigned)text.bytes[i]) < 0) return 74;
            }
            if (fputs("\",\"profileBound\":false}\n", output) == EOF) return 74;
        }
    }
    if (fprintf(output, "{\"status\":\"raw_report_complete\",\"attributeReads\":9,"
                "\"readOK\":%u,\"readNotOK\":%u,\"profileBound\":false,"
                "\"hashboardReady\":false}\n", ok, 9U - ok) < 0 ||
        fflush(output) != 0) return 74;

    return 0;
}

#ifndef HS_AML_READONLY_DIAGNOSTIC_NO_MAIN
int main(int argc, char **argv)
{
    return hs_aml_readonly_diagnostic_main(argc, argv, stdout);
}
#endif
