/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HASHSTAT_SUPERVISOR_H
#define HASHSTAT_SUPERVISOR_H
#include <stddef.h>
#include <stdint.h>

#define HS_SUPERVISOR_DEFAULT_PORT 4029U
#include "hashstat-version.h"
#define HS_SUPERVISOR_VERSION HASHSTAT_VERSION
#define HS_SUPERVISOR_MAX_CLIENTS 4U
#define HS_SUPERVISOR_MAX_REQUEST 1024U
#define HS_SUPERVISOR_MAX_RESPONSE 8192U
#define HS_SUPERVISOR_EXIT_RECOVERY 75

enum hs_supervisor_phase {
    HS_SUPERVISOR_WAITING_HARDWARE = 0, HS_SUPERVISOR_STOPPED,
    HS_SUPERVISOR_SHUTDOWN, HS_SUPERVISOR_RECOVERY
};
struct hs_supervisor_state { enum hs_supervisor_phase phase; };
#define HS_SUPERVISOR_STATE_INIT { HS_SUPERVISOR_WAITING_HARDWARE }
enum hs_supervisor_action {
    HS_SUPERVISOR_CONTINUE = 0, HS_SUPERVISOR_QUIT, HS_SUPERVISOR_RECOVER
};
enum hs_supervisor_response {
    HS_SUPERVISOR_ERROR = -1, HS_SUPERVISOR_RESPONSE = 0, HS_SUPERVISOR_NEED_MORE
};
struct hs_supervisor_config {
    unsigned port;
    unsigned timeout_ms;
    unsigned max_clients;

    void (*listening)(void *context, unsigned port);
    void *context;
};
#define HS_SUPERVISOR_CONFIG_INIT { HS_SUPERVISOR_DEFAULT_PORT, 1000U, 4U, NULL, NULL }

enum hs_supervisor_response hashstat_supervisor_handle(
    struct hs_supervisor_state *, const char *, size_t, uint64_t elapsed_ms,
    char *, size_t, size_t *, enum hs_supervisor_action *);

int hashstat_supervisor_run(const struct hs_supervisor_config *);

int hashstat_supervisor_main(int argc, char **argv);
#endif
