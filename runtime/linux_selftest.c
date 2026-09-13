/* SPDX-License-Identifier: GPL-3.0-only */

#include <stddef.h>
int hs_arm_probe(void);
int hs_linux_write(unsigned fd, const void *bytes, size_t length);
int hs_linux_selftest_main(void);

static int write_bounded(const char *text, size_t length)
{
    size_t offset = 0;
    unsigned retries = 0;
    while (offset < length) {
        int written = hs_linux_write(1U, text + offset, length - offset);
        if (written == -4 && retries++ < 8U) continue;
        if (written <= 0 || (size_t)written > length - offset) return -1;
        offset += (size_t)written;
    }
    return 0;
}

int hs_linux_selftest_main(void)
{
    static const char pass[] = "HASHSTAT AML88 SOURCE SELFTEST PASS; NO HARDWARE TEST\n";
    static const char fail[] = "HASHSTAT AML88 SOURCE SELFTEST FAIL\n";
    const int result = hs_arm_probe();
    if (result != 0) {
        (void)write_bounded(fail, sizeof(fail) - 1U);
        return result;
    }
    return write_bounded(pass, sizeof(pass) - 1U) == 0 ? 0 : 125;
}
