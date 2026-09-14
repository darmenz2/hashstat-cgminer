/* SPDX-License-Identifier: GPL-3.0-only */

#ifdef main
#undef main
#endif
#include "miner.h"
#include "hashstat-core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
extern bool hs_test_set_vmask(struct pool *, json_t *);
extern bool hs_test_parse_vmask(struct pool *, json_t *);
extern bool hs_test_parse_diff(struct pool *, json_t *);
extern bool hs_test_gen_stratum_work(struct pool *, struct work *);
extern void hs_test_share_lock_init(void);
extern bool hs_test_submit_queued(struct pool *, struct work *);
extern bool hs_test_parse_stratum_response(struct pool *, char *);
extern unsigned hs_test_handshake(void);
extern unsigned hs_test_aml88_bridge(void);
extern unsigned hs_test_aml88_config(void);
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
struct fast_peer { int socket; struct pool *pool; bool received; };
static void *receive_and_release(void *arg)
{
    struct fast_peer *peer = arg;
    char line[2048]; size_t used = 0;
    while (used < sizeof(line)) {
        ssize_t n = read(peer->socket, line + used, 1);
        if (n != 1) return NULL;
        if (line[used++] == '\n') {
            clear_stratum_shares(peer->pool);
            peer->received = true; return NULL;
        }
    }
    return NULL;
}
static void test_snapshots(struct pool *pool)
{
    CHECK(hashstat_apply_mask(pool, HS_CORE_BM1362_MASK));
    pool->rpc_user = "fixture.\"quoted\\worker";
    pool->n1_len = 4; pool->n2size = 4;
    pool->nonce1 = strdup("01020304"); pool->nonce1bin = calloc(4, 1);
    CHECK(pool->nonce1 && pool->nonce1bin);
    CHECK(hex2bin(pool->nonce1bin, pool->nonce1, 4));
    pool->sdiff = 1; pool->next_diff = pool->diff_after = 0;
    char notify[] = "{\"method\":\"mining.notify\",\"params\":[\"job-1\","
        "\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"0000000000000000000000000000000000000000000000000000000000000000000000000000000000\","
        "\"00000000\",[],\"20000000\",\"1d00ffff\",\"495fab29\",true]}";
    CHECK(parse_method(pool, notify));
    struct work work = {0};
    CHECK(hs_test_gen_stratum_work(pool, &work));
    CHECK(hashstat_work_current(&work));
    CHECK(work.hs_snapshot.base_version == 0x20000000 && work.hs_snapshot.actual_version == 0x20000000);

    memset(pool->vmask_001, 0xff, sizeof(pool->vmask_001));
    memset(pool->vmask_002, 'x', sizeof(pool->vmask_002));
    CHECK(hashstat_roll_work_version(&work, 0x2000));
    CHECK(work.hs_snapshot.actual_version == 0x20002000);
    CHECK(work.data[0] == 0x20 && work.data[2] == 0x20);
    unsigned char old_header[128]; memcpy(old_header, work.data, 128);
    CHECK(!hashstat_roll_work_version(&work, 1)); CHECK(!memcmp(old_header, work.data, 128));
    char encoded[2048], sentinel[4] = "abc";
    cg_rlock(&pool->data_lock);
    CHECK(hashstat_submit_json_locked(pool, &work, 17, encoded, sizeof(encoded)));
    CHECK(!hashstat_submit_json_locked(pool, &work, 17, sentinel, sizeof(sentinel)));
    cg_runlock(&pool->data_lock);
    CHECK(!strcmp(sentinel, "abc"));
    json_error_t error;
    json_t *request = json_loads(encoded, 0, &error); CHECK(request != NULL);
    json_t *params = json_object_get(request, "params");
    CHECK(json_array_size(params) == 6);
    CHECK(!strcmp(json_string_value(json_array_get(params, 0)), pool->rpc_user));
    CHECK(!strcmp(json_string_value(json_array_get(params, 5)), "00002000"));
    json_decref(request);

    cg_wlock(&pool->data_lock);
    CHECK(hashstat_note_job_locked(pool, "job-2", 0x20000000, false));
    cg_wunlock(&pool->data_lock);
    CHECK(hashstat_work_current(&work));

    int sockets[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pool->sock = sockets[0]; pool->stratum_active = true;
    CHECK(hs_test_submit_queued(pool, copy_work(&work))); CHECK(pool->sshares == 1);
    char wire[2048]; ssize_t bytes = read(sockets[1], wire, sizeof(wire) - 1);
    CHECK(bytes > 0); wire[bytes] = 0;
    CHECK(wire[bytes-1] == '\n' && strstr(wire, "00002000"));
    clear_stratum_shares(pool); CHECK(pool->sshares == 0);

    for (unsigned i = 0; i < 64; ++i) {
        struct fast_peer peer = {sockets[1], pool, false}; pthread_t reader;
        CHECK(pthread_create(&reader, NULL, receive_and_release, &peer) == 0);
        CHECK(hs_test_submit_queued(pool, copy_work(&work)));
        CHECK(pthread_join(reader, NULL) == 0);
        CHECK(peer.received && pool->sshares == 0);
    }
    CHECK(hashstat_apply_mask(pool, HS_CORE_BM1362_MASK));
    CHECK(hashstat_work_current(&work));

    cg_wlock(&pool->data_lock);
    CHECK(hashstat_note_job_locked(pool, "job-1", 0x20000000, false));
    cg_wunlock(&pool->data_lock);
    CHECK(!hashstat_work_current(&work));
    CHECK(!hs_test_submit_queued(pool, copy_work(&work))); CHECK(pool->sshares == 0);

    clean_work(&work); CHECK(parse_method(pool, notify));
    CHECK(hs_test_gen_stratum_work(pool, &work));
    struct work *copy = copy_work(&work);
    CHECK(copy && copy->job_id != work.job_id && !strcmp(copy->job_id, work.job_id));
    CHECK(!memcmp(&copy->hs_snapshot, &work.hs_snapshot, sizeof(work.hs_snapshot)));
    CHECK(hashstat_apply_mask(pool, 0)); CHECK(!hashstat_work_current(copy)); free_work(copy);
    CHECK(!hashstat_work_current(&work));
    clean_work(&work); CHECK(hs_test_gen_stratum_work(pool, &work));
    CHECK(hashstat_work_current(&work));
    CHECK(hashstat_roll_work_version(&work, 0));
    CHECK(!hashstat_roll_work_version(&work, 0x2000));

    CHECK(parse_method(pool, notify)); CHECK(!hashstat_work_current(&work));
    clean_work(&work); CHECK(hs_test_gen_stratum_work(pool, &work));
    uint64_t session = work.hs_snapshot.session_epoch;
    mutex_lock(&pool->stratum_lock); cg_wlock(&pool->data_lock);
    hashstat_session_reset_locked(pool);
    cg_wunlock(&pool->data_lock); mutex_unlock(&pool->stratum_lock);
    CHECK(pool->hs_stratum.session_epoch != session); CHECK(!hashstat_work_current(&work));
    CHECK(hashstat_apply_configure(pool, false, 0));
    pool->hs_stratum.ready = pool->hs_stratum.authorized = true;
    CHECK(parse_method(pool, notify));
    CHECK(!hashstat_work_current(&work));
    clean_work(&work); CHECK(hs_test_gen_stratum_work(pool, &work));
    cg_rlock(&pool->data_lock);
    CHECK(hashstat_submit_json_locked(pool, &work, 18, encoded, sizeof(encoded)));
    cg_runlock(&pool->data_lock);
    request = json_loads(encoded, 0, &error); CHECK(request != NULL);
    CHECK(json_array_size(json_object_get(request, "params")) == 5); json_decref(request);
    CHECK(!hashstat_roll_work_version(&work, 0));

    const char *saved_worker = pool->rpc_user; pool->rpc_user = "changed.config";
    cg_rlock(&pool->data_lock);
    CHECK(hashstat_submit_json_locked(pool, &work, 19, encoded, sizeof(encoded)));
    cg_runlock(&pool->data_lock);
    CHECK(strstr(encoded, "changed.config") == NULL); pool->rpc_user = (char *)saved_worker;
    uint64_t saved_nonce2 = work.nonce2; work.nonce2 = UINT64_C(1) << 32;
    cg_rlock(&pool->data_lock);
    CHECK(!hashstat_submit_json_locked(pool, &work, 20, encoded, sizeof(encoded)));
    cg_runlock(&pool->data_lock); work.nonce2 = saved_nonce2;
    struct work empty_work = {0}; pool->nonce2 = UINT64_C(1) << 32;
    CHECK(!hs_test_gen_stratum_work(pool, &empty_work));
    CHECK(!empty_work.hs_snapshot.valid); pool->nonce2 = 0;

    CHECK(close(sockets[1]) == 0); sockets[1] = -1;
    CHECK(!hs_test_submit_queued(pool, copy_work(&work)));
    CHECK(pool->sshares == 0 && !pool->hs_stratum.ready);
    CHECK(!hashstat_work_current(&work));
    sockets[0] = -1;
    CHECK(hashstat_apply_configure(pool, false, 0));
    pool->hs_stratum.ready = pool->hs_stratum.authorized = true;
    CHECK(parse_method(pool, notify)); clean_work(&work);
    CHECK(hs_test_gen_stratum_work(pool, &work));

    cg_wlock(&pool->data_lock);
    for (unsigned i = 0; i < HS_STRATUM_JOB_SLOTS; ++i) {
        char id[32]; snprintf(id, sizeof(id), "new-job-%u", i);
        CHECK(hashstat_note_job_locked(pool, id, 0x20000000, false));
    }
    cg_wunlock(&pool->data_lock);
    CHECK(!hashstat_work_current(&work));

    char unknown[] = "{\"id\":1234567,\"result\":true,\"error\":null}";
    int64_t accepted = pool->accepted;
    CHECK(!hs_test_parse_stratum_response(pool, unknown)); CHECK(pool->accepted == accepted);
    cg_wlock(&pool->data_lock);
    pool->hs_stratum.next_job_sequence = UINT64_MAX;
    CHECK(!hashstat_note_job_locked(pool, "overflow", 0, false));
    CHECK(pool->hs_stratum.exhausted && !pool->hs_stratum.ready);
    cg_wunlock(&pool->data_lock);
    clean_work(&work); pool->sock = 0;
    free(pool->nonce1); pool->nonce1 = NULL; free(pool->nonce1bin); pool->nonce1bin = NULL;
    free(pool->coinbase); pool->coinbase = NULL; free(pool->swork.job_id); pool->swork.job_id = NULL;
    free(pool->swork.merkle_bin); pool->swork.merkle_bin = NULL;
}
int main(void)
{
    struct pool pool = {0};

    CHECK(pthread_mutex_init(&console_lock, NULL) == 0);
    cglock_init(&pool.data_lock);
    cglock_init(&control_lock);
    CHECK(pthread_mutex_init(&pool.stratum_lock, NULL) == 0);
    hs_test_share_lock_init();
    signal(SIGPIPE, SIG_IGN);
    hashstat_session_reset_locked(&pool);
    CHECK(hashstat_apply_configure(&pool, true, HS_CORE_BM1362_MASK));
    pool.hs_stratum.ready = pool.hs_stratum.authorized = true;
    const char *bad_masks[] = {"null", "true", "123", "{}", "[]", "\"\"",
        "\"1fffe00\"", "\"1fffe0000\"", "\"1fffe00z\"", "\"80000000\"",
        "\"ffffffff\"", "\"00000000\"", "\"00002001\"", "\"0000\\u0000000\""};
    json_error_t error;
    for (size_t i = 0; i < sizeof(bad_masks)/sizeof(*bad_masks); ++i) {
        json_t *value = json_loads(bad_masks[i], JSON_DECODE_ANY | JSON_ALLOW_NUL, &error);
        CHECK(value != NULL);
        pool.vmask_003[0] = 17; pool.vmask_003[1] = 18; pool.vmask_003[2] = 19;
        CHECK(!hs_test_set_vmask(&pool, value));
        CHECK(pool.vmask_003[0] == 17 && pool.vmask_003[1] == 18 && pool.vmask_003[2] == 19);
        json_decref(value);
    }
    CHECK(!hs_test_set_vmask(&pool, NULL));
    CHECK(!hs_test_set_vmask(NULL, NULL));
    CHECK(!hs_test_parse_vmask(NULL, NULL));
    CHECK(!hs_test_parse_diff(NULL, NULL));
    json_t *value = json_string("1fffe000");
    CHECK(hs_test_set_vmask(&pool, value)); json_decref(value);
    CHECK(pool.vmask_003[0] == 0x1fffe000 && pool.vmask_003[1] == 0x1fff0000 && pool.vmask_003[2] == 0xe000);
    const char *invalid_diff[] = {"null", "true", "1", "{}", "[]", "[null]", "[true]", "[\"1\"]",
        "[0]", "[-1]", "[1e-100]", "[1e100]", "[1,2]"};
    for (size_t i = 0; i < sizeof(invalid_diff)/sizeof(*invalid_diff); ++i) {
        value = json_loads(invalid_diff[i], JSON_DECODE_ANY, &error); CHECK(value != NULL);
        pool.next_diff = 0; pool.diff_after = 13; pool.sdiff = 17;
        CHECK(!hs_test_parse_diff(&pool, value));
        CHECK(pool.next_diff == 0 && pool.diff_after == 13 && pool.sdiff == 17);
        json_decref(value);
    }
    value = json_loads("[65536.125]", 0, &error); CHECK(value != NULL);
    CHECK(hs_test_parse_diff(&pool, value)); CHECK(pool.next_diff == 65536.125);
    CHECK(pool.sdiff == 17 && pool.diff_after == 13); json_decref(value);
    value = json_loads("[1024]", 0, &error); CHECK(value != NULL);
    CHECK(hs_test_parse_diff(&pool, value)); CHECK(pool.diff_after == 1024); json_decref(value);
    pool.vmask = true;
    value = json_loads("[\"1fffe00z\"]", 0, &error); CHECK(value != NULL);
    CHECK(!hs_test_parse_vmask(&pool, value)); CHECK(pool.vmask); json_decref(value);
    value = json_loads("[\"00002000\",\"00004000\"]", 0, &error); CHECK(value != NULL);
    CHECK(!hs_test_parse_vmask(&pool, value)); json_decref(value);
    value = json_loads("[\"00002000\"]", 0, &error); CHECK(value != NULL);
    CHECK(hs_test_parse_vmask(&pool, value)); CHECK(pool.hs_stratum.mask == 0x2000); json_decref(value);

    char invalid_json[] = "{bad json";
    CHECK(!parse_method(&pool, invalid_json));
    char invalid_difficulty[] = "{\"method\":\"mining.set_difficulty\",\"params\":[\"1\"]}";
    double previous_next = pool.next_diff, previous_after = pool.diff_after;
    CHECK(!parse_method(&pool, invalid_difficulty));
    CHECK(pool.next_diff == previous_next && pool.diff_after == previous_after);
    char valid_difficulty[] = "{\"method\":\"mining.set_difficulty\",\"params\":[8192],\"error\":null}";
    CHECK(parse_method(&pool, valid_difficulty)); CHECK(pool.diff_after == 8192);
    char invalid_mask[] = "{\"method\":\"mining.set_version_mask\",\"params\":[\"ffffffff\"]}";
    CHECK(!parse_method(&pool, invalid_mask)); CHECK(pool.hs_stratum.mask == 0x2000);
    char valid_mask[] = "{\"method\":\"mining.set_version_mask\",\"params\":[\"00004000\"]}";
    CHECK(parse_method(&pool, valid_mask)); CHECK(pool.hs_stratum.mask == 0x4000);
    char explicit_error[] = "{\"method\":\"mining.set_difficulty\",\"params\":[1],\"error\":[20,\"fixture rejection\",null]}";
    CHECK(!parse_method(&pool, explicit_error)); CHECK(pool.diff_after == 8192);
    test_snapshots(&pool);
    checks += hs_test_handshake();
    checks += hs_test_aml88_bridge();
    checks += hs_test_aml88_config();
    CHECK(pthread_mutex_destroy(&console_lock) == 0);
    printf("PASS %u real upstream Stratum parser/work assertions; no external connections\n", checks);
    return 0;
}
