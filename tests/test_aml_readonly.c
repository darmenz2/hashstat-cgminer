/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "hs_aml_readonly.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(c) do { assert(c); ++checks; } while (0)
static struct fixture {
    const char *path;
    uint8_t data[8];
    size_t length, offset, chunk;
    unsigned open_calls, stat_calls, read_calls, close_calls;
    unsigned open_eintr, read_eintr;
    unsigned read_error_at, late_eintr_at;
    int open_error, stat_error, read_error, close_error, oversized;
    mode_t mode;
} f;
static int mock_open(const char *path, int flags, ...)
{
    ++f.open_calls;
    CHECK(strcmp(path, f.path) == 0);
    CHECK((flags & O_ACCMODE) == O_RDONLY);
    CHECK((flags & (O_CREAT | O_TRUNC | O_APPEND)) == 0);
    CHECK((flags & (O_CLOEXEC | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW)) ==
          (O_CLOEXEC | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW));
    if (f.open_calls <= f.open_eintr) { errno = EINTR; return -1; }
    if (f.open_error) { errno = f.open_error; return -1; }
    return 123;
}
static int mock_fstat(int fd, struct stat *s)
{
    ++f.stat_calls; CHECK(fd == 123);
    if (f.stat_error) { errno = EIO; return -1; }
    memset(s, 0, sizeof(*s)); s->st_mode = f.mode;
    return 0;
}
static ssize_t mock_read(int fd, void *out, size_t length)
{
    ++f.read_calls;
    CHECK(fd == 123 && length > 0 && length <= 5U);
    if (f.read_calls <= f.read_eintr) { errno = EINTR; return -1; }
    if (f.late_eintr_at != 0 && f.read_calls == f.late_eintr_at) { errno = EINTR; return -1; }
    if (f.read_error && (f.read_error_at == 0 || f.read_calls >= f.read_error_at)) {
        errno = f.read_error; return -1;
    }
    if (f.oversized) return (ssize_t)length + 1;
    size_t n = f.length - f.offset;
    if (n > length) n = length;
    if (n > f.chunk) n = f.chunk;
    memcpy(out, f.data + f.offset, n); f.offset += n;
    return (ssize_t)n;
}
static int mock_close(int fd)
{
    ++f.close_calls; CHECK(fd == 123 && f.close_calls == 1U);
    if (f.close_error) { errno = f.close_error; return -1; }
    return 0;
}
#define open mock_open
#define fstat mock_fstat
#define read mock_read
#define close mock_close
#include "../runtime/aml_readonly.c"
#undef open
#undef fstat
#undef read
#undef close

static void initialize(void)
{
    memset(&f, 0, sizeof(f));
    f.path = "/sys/class/gpio/gpio439/value";
    memcpy(f.data, "1\n", 2); f.length = 2; f.chunk = 8; f.mode = S_IFREG | 0444;
}
static struct hs_aml_text run(void)
{
    return hs_aml_readonly_attribute(NULL, 0, HS_AML_ATTRIBUTE_VALUE);
}
static void clean_error(struct hs_aml_text result, enum hs_aml_text_status expected)
{
    CHECK(result.status == expected && result.length == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(result.bytes[i] == 0);
}
int main(void)
{
    struct hs_aml_text result;
    initialize(); result = run();
    CHECK(result.status == HS_AML_TEXT_OK && result.length == 2 && result.bytes[0] == '1');
    CHECK(f.open_calls == 1 && f.stat_calls == 1 && f.read_calls == 2 && f.close_calls == 1);
    for (unsigned chain = 0; chain < 3U; ++chain)
        for (unsigned attr = 0; attr < 3U; ++attr) {
            char path[96]; const char *const attrs[3] = {"direction", "active_low", "value"};
            initialize();
            CHECK(snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/%s", 439U + chain, attrs[attr]) > 0);
            f.path = path;
            result = hs_aml_readonly_attribute(NULL, chain, (enum hs_aml_attribute)attr);
            CHECK(result.status == HS_AML_TEXT_OK && f.close_calls == 1);
        }
    initialize(); result = hs_aml_readonly_attribute(NULL, 3, HS_AML_ATTRIBUTE_VALUE);
    clean_error(result, HS_AML_TEXT_UNAVAILABLE); CHECK(f.open_calls == 0);
    result = hs_aml_readonly_attribute(NULL, 0, (enum hs_aml_attribute)-1);
    clean_error(result, HS_AML_TEXT_UNAVAILABLE); CHECK(f.open_calls == 0);
    for (unsigned chunk = 1; chunk <= 5U; ++chunk)
        for (unsigned length = 0; length <= 8U; ++length) {
            initialize(); f.chunk = chunk; f.length = length;
            result = run();
            if (length <= 4U) {
                CHECK(result.status == HS_AML_TEXT_OK && result.length == length);
                CHECK(memcmp(result.bytes, f.data, length) == 0);
            } else clean_error(result, HS_AML_TEXT_TOO_LONG);
            CHECK(f.close_calls == 1 && f.read_calls <= 16U);
        }
    for (unsigned n = 0; n <= 12U; ++n) {
        initialize(); f.open_eintr = n; result = run();
        if (n < 9U) CHECK(result.status == HS_AML_TEXT_OK && f.close_calls == 1);
        else { clean_error(result, HS_AML_TEXT_READ_ERROR); CHECK(f.close_calls == 0); }
        CHECK(f.open_calls <= 9U);
    }
    for (unsigned n = 0; n <= 20U; ++n) {
        initialize(); f.read_eintr = n; result = run();
        if (n < 15U) CHECK(result.status == HS_AML_TEXT_OK);
        else clean_error(result, HS_AML_TEXT_READ_ERROR);
        CHECK(f.read_calls <= 16U && f.close_calls == 1);
    }
    initialize(); f.open_error = ENOENT; clean_error(run(), HS_AML_TEXT_UNAVAILABLE);
    CHECK(f.read_calls == 0 && f.close_calls == 0);
    initialize(); f.open_error = EACCES; clean_error(run(), HS_AML_TEXT_READ_ERROR);
    CHECK(f.read_calls == 0 && f.close_calls == 0);
    initialize(); f.stat_error = 1; clean_error(run(), HS_AML_TEXT_READ_ERROR);
    CHECK(f.read_calls == 0 && f.close_calls == 1);
    initialize(); f.mode = S_IFCHR | 0444; clean_error(run(), HS_AML_TEXT_UNSAFE_TYPE);
    CHECK(f.read_calls == 0 && f.close_calls == 1);
    initialize(); f.read_error = EIO; clean_error(run(), HS_AML_TEXT_READ_ERROR);
    CHECK(f.close_calls == 1);
    initialize(); f.read_error = EAGAIN; clean_error(run(), HS_AML_TEXT_READ_ERROR);
    CHECK(f.read_calls == 1 && f.close_calls == 1);
    initialize(); f.oversized = 1; clean_error(run(), HS_AML_TEXT_READ_ERROR);
    CHECK(f.close_calls == 1);
    initialize(); f.close_error = EINTR; clean_error(run(), HS_AML_TEXT_READ_ERROR);
    CHECK(f.close_calls == 1);
    initialize(); f.close_error = EIO; clean_error(run(), HS_AML_TEXT_READ_ERROR);
    CHECK(f.close_calls == 1 && f.read_calls == 2);
    const int errors[2] = {EIO, EAGAIN};
    for (unsigned i = 0; i < 2; ++i) {
        initialize(); f.chunk = 1; f.read_error = errors[i]; f.read_error_at = 2;
        clean_error(run(), HS_AML_TEXT_READ_ERROR);
        CHECK(f.offset == 1 && f.read_calls == 2 && f.close_calls == 1);
    }
    initialize(); f.chunk = 1; f.late_eintr_at = 2;
    result = run();
    CHECK(result.status == HS_AML_TEXT_OK && result.length == 2 &&
          result.bytes[0] == '1' && result.bytes[1] == '\n');
    CHECK(f.read_calls == 4 && f.close_calls == 1);
    printf("AML read-only POSIX adapter: %u checks PASS; all OS calls mocked\n", checks);
    return 0;
}
