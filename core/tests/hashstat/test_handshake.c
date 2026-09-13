/* SPDX-License-Identifier: GPL-3.0-only */

#include "miner.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern bool hs_test_gen_stratum_work(struct pool *, struct work *);

static unsigned assertions;
#define REQUIRE(x) do { ++assertions; if (!(x)) { \
    fprintf(stderr, "HANDSHAKE FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static const char notification[] = "{\"id\":null,\"method\":\"mining.notify\",\"params\":[\"early-job\","
    "\"0000000000000000000000000000000000000000000000000000000000000000\","
    "\"0000000000000000000000000000000000000000000000000000000000000000000000000000000000\","
    "\"00000000\",[],\"20000000\",\"1d00ffff\",\"495fab29\",true]}\n";
enum scenario {
    EARLY_CONFIG, EARLY_SUBSCRIBE, EARLY_AUTH, ZERO_MASK, NO_ROLLING,
    CONFIG_FALSE, CONFIG_WRONG_ID, CONFIG_STRING_ID, CONFIG_DUPLICATE_ID,
    CONFIG_MASK_NUMBER, CONFIG_MASK_OUTSIDE, CONFIG_MASK_MISSING,
    SUBSCRIBE_WRONG_ID, SUBSCRIBE_SIZE_FLOAT, SUBSCRIBE_NONCE_NUL,
    AUTH_FALSE, AUTH_WRONG_ID, AUTH_RESULT_NUMBER, AUTH_ERROR,
    CONFIG_PARTIAL, AUTH_PARTIAL, CONFIG_OVERSIZED, CONFIG_NUL,
    CONFIG_STORM, AUTH_STORM, UNKNOWN_METHOD, METHOD_SUFFIX,
    SCENARIO_COUNT
};
struct handshake_peer { int listener; enum scenario scenario; bool ok; };
static bool peer_write(int fd, const char *data)
{
    size_t n = strlen(data), used = 0;
    while (used < n) {
        struct pollfd pollfd = {fd, POLLOUT, 0};
        if (poll(&pollfd, 1, 1000) != 1) return false;
        ssize_t sent = send(fd, data + used, n - used, MSG_DONTWAIT);
        if (sent <= 0) return false;
        used += (size_t)sent;
    }
    return true;
}
static int peer_request(int fd, const char *method)
{
    char line[2048]; size_t used = 0;
    while (used < sizeof(line) - 1) {
        struct pollfd pollfd = {fd, POLLIN, 0};
        if (poll(&pollfd, 1, 1000) != 1) return -1;
        ssize_t n = recv(fd, line + used, 1, MSG_DONTWAIT);
        if (n != 1) return -1;
        if (line[used++] == '\n') break;
    }
    if (!used || line[used - 1] != '\n') return -1;
    line[used] = 0;
    json_error_t error; json_t *request = json_loads(line, JSON_REJECT_DUPLICATES, &error);
    json_t *id = json_object_get(request, "id");
    const char *name = json_string_value(json_object_get(request, "method"));
    int result = json_is_integer(id) && name && !strcmp(name, method) ?
        (int)json_integer_value(id) : -1;
    if (result >= 0 && !strcmp(method, "mining.authorize")) {
        json_t *params = json_object_get(request, "params");
        const char *user = json_string_value(json_array_get(params, 0));
        const char *password = json_string_value(json_array_get(params, 1));
        if (!user || !password || strcmp(user, "local.\"fixture\\worker") ||
            strcmp(password, "not-a-\"real\\password")) result = -1;
    }
    json_decref(request); return result;
}
static void *handshake_peer_main(void *arg)
{
    struct handshake_peer *peer = arg;
    struct pollfd p = {peer->listener, POLLIN, 0};
    if (poll(&p, 1, 1000) != 1) return NULL;
    int fd = accept(peer->listener, NULL, NULL);
    if (fd < 0) return NULL;
    char reply[512];
    int configure = peer_request(fd, "mining.configure");
    if (configure < 0) goto done;
    if (peer->scenario == CONFIG_PARTIAL) {
        if (!peer_write(fd, "{\"id\":")) goto done;
        struct pollfd held = {fd, POLLIN, 0}; (void)poll(&held, 1, 800); goto done;
    }
    if (peer->scenario == CONFIG_NUL) {
        const char zero[] = "{\"id\":\0false}\n";
        (void)send(fd, zero, sizeof(zero) - 1, MSG_DONTWAIT); goto done;
    }
    if (peer->scenario == CONFIG_OVERSIZED) {
        char chunk[2049]; memset(chunk, ' ', sizeof(chunk) - 1); chunk[2048] = 0;
        for (unsigned i = 0; i < 18; ++i) if (!peer_write(fd, chunk)) break;
        goto done;
    }
    if (peer->scenario == CONFIG_STORM) {
        for (unsigned i = 0; i < 34; ++i) if (!peer_write(fd, notification)) break;
        goto done;
    }
    if (peer->scenario == UNKNOWN_METHOD || peer->scenario == METHOD_SUFFIX) {
        (void)peer_write(fd, peer->scenario == UNKNOWN_METHOD ?
            "{\"method\":\"client.reconnect\",\"params\":[\"external.invalid\",3333]}\n" :
            "{\"method\":\"mining.set_difficulty.suffix\",\"params\":[1]}\n");
        goto done;
    }
    if (peer->scenario == EARLY_CONFIG && !peer_write(fd, notification)) goto done;
    snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"version-rolling\":true,\"version-rolling.mask\":\"1fffe000\"},\"error\":null}\n", configure);
    if (peer->scenario == ZERO_MASK)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"version-rolling\":true,\"version-rolling.mask\":\"00000000\"}}\n", configure);
    if (peer->scenario == NO_ROLLING)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"version-rolling\":false}}\n", configure);
    if (peer->scenario == CONFIG_FALSE)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":false}\n", configure);
    if (peer->scenario == CONFIG_WRONG_ID)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"version-rolling\":false}}\n", configure + 1);
    if (peer->scenario == CONFIG_STRING_ID)
        snprintf(reply, sizeof(reply), "{\"id\":\"%d\",\"result\":{\"version-rolling\":false}}\n", configure);
    if (peer->scenario == CONFIG_DUPLICATE_ID)
        snprintf(reply, sizeof(reply), "{\"id\":0,\"id\":%d,\"result\":{\"version-rolling\":false}}\n", configure);
    if (peer->scenario == CONFIG_MASK_NUMBER)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"version-rolling\":true,\"version-rolling.mask\":1}}\n", configure);
    if (peer->scenario == CONFIG_MASK_OUTSIDE)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"version-rolling\":true,\"version-rolling.mask\":\"ffffffff\"}}\n", configure);
    if (peer->scenario == CONFIG_MASK_MISSING)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":{\"version-rolling\":true}}\n", configure);
    if (!peer_write(fd, reply)) goto done;
    if (peer->scenario >= CONFIG_FALSE && peer->scenario <= CONFIG_MASK_MISSING) goto done;
    int subscribe = peer_request(fd, "mining.subscribe");
    if (subscribe < 0) goto done;
    if (peer->scenario == EARLY_SUBSCRIBE) {
        if (!peer_write(fd, "{\"method\":\"mining.set_difficulty\",\"params\":[8192]}\n") ||
            !peer_write(fd, notification)) goto done;
    }
    snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":[[[\"mining.notify\",\"fixture-session\"]],\"01020304\",4],\"error\":null}\n", subscribe);
    if (peer->scenario == SUBSCRIBE_WRONG_ID)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":[[],\"01020304\",4]}\n", subscribe + 1);
    if (peer->scenario == SUBSCRIBE_SIZE_FLOAT)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":[[],\"01020304\",4.0]}\n", subscribe);
    if (peer->scenario == SUBSCRIBE_NONCE_NUL)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":[[],\"01\\u000002\",4]}\n", subscribe);
    if (!peer_write(fd, reply)) goto done;
    if (peer->scenario >= SUBSCRIBE_WRONG_ID && peer->scenario <= SUBSCRIBE_NONCE_NUL) goto done;
    int auth = peer_request(fd, "mining.authorize");
    if (auth < 0) goto done;
    if (peer->scenario == AUTH_PARTIAL) {
        if (!peer_write(fd, "{\"id\":")) goto done;
        struct pollfd held = {fd, POLLIN, 0}; (void)poll(&held, 1, 800); goto done;
    }
    if (peer->scenario == AUTH_STORM) {
        for (unsigned i = 0; i < 66; ++i)
            if (!peer_write(fd, "{\"method\":\"mining.set_difficulty\",\"params\":[1]}\n")) break;
        goto done;
    }
    if (peer->scenario == EARLY_AUTH && !peer_write(fd, notification)) goto done;
    snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":true,\"error\":null}\n", auth);
    if (peer->scenario == AUTH_FALSE)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":false,\"error\":null}\n", auth);
    if (peer->scenario == AUTH_WRONG_ID)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":true}\n", auth + 1);
    if (peer->scenario == AUTH_RESULT_NUMBER)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":1}\n", auth);
    if (peer->scenario == AUTH_ERROR)
        snprintf(reply, sizeof(reply), "{\"id\":%d,\"result\":true,\"error\":[24,\"denied\",null]}\n", auth);
    peer->ok = peer_write(fd, reply);
done:
    close(fd); return NULL;
}
unsigned hs_test_handshake(void)
{
    for (enum scenario scenario = EARLY_CONFIG; scenario < SCENARIO_COUNT; ++scenario) {
    struct pool pool = {0}; struct handshake_peer peer = {0};
    peer.scenario = scenario;
    cglock_init(&pool.data_lock);
    REQUIRE(pthread_mutex_init(&pool.stratum_lock, NULL) == 0);
    peer.listener = socket(AF_INET, SOCK_STREAM, 0); REQUIRE(peer.listener > 0);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(peer.listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(getsockname(peer.listener, (struct sockaddr *)&address, &size) == 0);
    REQUIRE(listen(peer.listener, 1) == 0);
    char port[16]; snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    pool.sockaddr_url = "127.0.0.1"; pool.stratum_port = port;
    pool.rpc_user = "local.\"fixture\\worker"; pool.rpc_pass = "not-a-\"real\\password";
    pthread_t worker; REQUIRE(pthread_create(&worker, NULL, handshake_peer_main, &peer) == 0);
    bool init_expected = scenario <= NO_ROLLING ||
        (scenario >= AUTH_FALSE && scenario <= AUTH_ERROR) ||
        scenario == AUTH_PARTIAL || scenario == AUTH_STORM;
    bool auth_expected = scenario <= NO_ROLLING;
    struct timespec started, finished; REQUIRE(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    bool initiated = initiate_stratum(&pool);
    if (initiated != init_expected) fprintf(stderr, "scenario %d initiate=%d expected=%d\n", scenario, initiated, init_expected);
    REQUIRE(initiated == init_expected);
    if (initiated) {
    REQUIRE(pool.hs_stratum.ready && !pool.hs_stratum.authorized);
    if (scenario == EARLY_CONFIG || scenario == EARLY_SUBSCRIBE)
        REQUIRE(pool.stratum_notify && pool.swork.job_id && !strcmp(pool.swork.job_id, "early-job"));
    if (scenario == EARLY_SUBSCRIBE) REQUIRE(pool.sdiff == 8192);
    REQUIRE(auth_stratum(&pool) == auth_expected);
    REQUIRE(pool.hs_stratum.authorized == auth_expected);
    if (scenario == EARLY_AUTH)
        REQUIRE(pool.stratum_notify && pool.swork.job_id && !strcmp(pool.swork.job_id, "early-job"));
    if (scenario == ZERO_MASK) REQUIRE(pool.hs_stratum.rolling && pool.hs_stratum.mask == 0);
    if (scenario == NO_ROLLING) REQUIRE(!pool.hs_stratum.rolling && pool.hs_stratum.mask == 0);
    }
    if (!auth_expected) REQUIRE(!pool.stratum_active && !pool.hs_stratum.ready && !pool.sock);
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
    int64_t elapsed_ms = (int64_t)(finished.tv_sec - started.tv_sec) * 1000 +
        (finished.tv_nsec - started.tv_nsec) / 1000000;
    REQUIRE(elapsed_ms >= 0 && elapsed_ms < 1500);
    if (scenario == CONFIG_PARTIAL || scenario == AUTH_PARTIAL)
        REQUIRE(elapsed_ms >= 150 && elapsed_ms < 650);
    REQUIRE(pthread_join(worker, NULL) == 0);
    if (auth_expected) REQUIRE(peer.ok);
    if (scenario == EARLY_CONFIG) {
        struct work previous = {0};
        REQUIRE(hs_test_gen_stratum_work(&pool, &previous));
        REQUIRE(hashstat_work_current(&previous));
        uint64_t old_epoch = pool.hs_stratum.session_epoch;

        REQUIRE(pool.sockbuf_size > 80);
        strcpy(pool.sockbuf, "{\"id\":1,\"result\":true}\n");
        peer.ok = false;
        REQUIRE(pthread_create(&worker, NULL, handshake_peer_main, &peer) == 0);
        REQUIRE(initiate_stratum(&pool));
        REQUIRE(pool.hs_stratum.session_epoch > old_epoch);
        REQUIRE(!pool.hs_stratum.authorized && !hashstat_work_current(&previous));
        REQUIRE(auth_stratum(&pool));
        REQUIRE(pool.hs_stratum.authorized && !hashstat_work_current(&previous));
        REQUIRE(pthread_join(worker, NULL) == 0 && peer.ok);
        clean_work(&previous);
    }
    suspend_stratum(&pool);
    REQUIRE(close(peer.listener) == 0);
    free(pool.sockbuf); free(pool.nonce1); free(pool.nonce1bin); free(pool.sessionid);
    free(pool.coinbase); free(pool.swork.job_id);
    for (int i = 0; i < pool.merkles; ++i) free(pool.swork.merkle_bin[i]);
    free(pool.swork.merkle_bin);
    cglock_destroy(&pool.data_lock);
    REQUIRE(pthread_mutex_destroy(&pool.stratum_lock) == 0);
    }
    return assertions;
}
