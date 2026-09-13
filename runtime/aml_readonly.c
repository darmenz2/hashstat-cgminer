/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "hs_aml_readonly.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

struct hs_aml_text hs_aml_readonly_attribute(void *context, unsigned chain,
                                           enum hs_aml_attribute attribute)
{
    (void)context;
    struct hs_aml_text result = {0};
    const char *const names[3] = {"direction", "active_low", "value"};
    char path[96];
    uint32_t gpio;
    if ((unsigned)attribute > HS_AML_ATTRIBUTE_VALUE ||
        !hs_aml_presence_gpio_for_chain(chain, &gpio)) return result;
    const int count = snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/%s",
                               (unsigned)gpio, names[(unsigned)attribute]);
    if (count <= 0 || (size_t)count >= sizeof(path)) return result;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 9U; ++attempt) {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW);
        if (fd >= 0 || errno != EINTR) break;
    }
    if (fd < 0) {
        result.status = errno == ENOENT ? HS_AML_TEXT_UNAVAILABLE : HS_AML_TEXT_READ_ERROR;
        return result;
    }
    struct stat info;
    result.status = HS_AML_TEXT_READ_ERROR;
    if (fstat(fd, &info) != 0) goto finished;
    if (!S_ISREG(info.st_mode)) {
        result.status = HS_AML_TEXT_UNSAFE_TYPE;
        goto finished;
    }
    uint8_t bytes[HS_AML_OBSERVE_TEXT_CAPACITY + 1U];
    size_t used = 0;

    for (unsigned attempt = 0; attempt < 16U; ++attempt) {
        const ssize_t count_read = read(fd, bytes + used, sizeof(bytes) - used);
        if (count_read < 0) {
            if (errno == EINTR) continue;
            goto finished;
        }
        if (count_read == 0) {
            result.status = HS_AML_TEXT_OK;
            result.length = used;
            for (size_t i = 0; i < used; ++i) result.bytes[i] = bytes[i];
            goto finished;
        }
        if ((size_t)count_read > sizeof(bytes) - used) goto finished;
        used += (size_t)count_read;
        if (used > HS_AML_OBSERVE_TEXT_CAPACITY) {
            result.status = HS_AML_TEXT_TOO_LONG;
            goto finished;
        }
    }
finished:

    if (close(fd) != 0) result.status = HS_AML_TEXT_READ_ERROR;
    if (result.status != HS_AML_TEXT_OK) {
        result.length = 0;
        for (unsigned i = 0; i < HS_AML_OBSERVE_TEXT_CAPACITY; ++i) result.bytes[i] = 0;
    }
    return result;
}
