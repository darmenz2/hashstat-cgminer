/* SPDX-License-Identifier: GPL-3.0-only */

#ifdef main
#undef main
#endif
#include "miner.h"
#include "hashstat-core.h"
#include "hashstat-aml88-core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error "AML88 bridge assertions must remain active"
#endif

static unsigned checks, clones, releases, submitted;
#define B(x) do { ++checks; assert(x); } while (0)
static const uint8_t capture[11] = {0xaa,0x55,0x4c,0x03,0x52,0x75,0x0c,0xd2,0x05,0xa2,0x9c};
struct mock_uart {
    struct hs_aml_uart_termios2 termios;
    struct hs_aml88_bridge *bridge;
    struct pool *pool;
    uint64_t now;
    unsigned closes, writes, short_error, change_epoch;
    size_t bytes;
};
static int mock_open(void *p, const char *path, uint32_t flags, enum hs_aml_uart_io_error *e)
{
    (void)p; (void)e; B(!strcmp(path,"/dev/ttyS3")); B(flags == HS_AML_UART_OPEN_FLAGS); return 7;
}
static int mock_verify(void *p, int fd, unsigned chain, enum hs_aml_uart_io_error *e)
{ (void)p; (void)e; B(fd == 7 && chain == 0); return 0; }
static int mock_control(void *p, int fd, uint32_t request, void *arg, enum hs_aml_uart_io_error *e)
{
    struct mock_uart *m=p; (void)e; B(fd == 7);
    if (request == HS_AML_UART_TCGETS2) *(struct hs_aml_uart_termios2 *)arg=m->termios;
    else if (request == HS_AML_UART_TCSETS2) m->termios=*(struct hs_aml_uart_termios2 *)arg;
    else B(request == HS_AML_UART_TIOCEXCL || request == HS_AML_UART_TIOCNXCL);
    return 0;
}
static ptrdiff_t mock_read(void *p, int fd, uint8_t *data, size_t n, enum hs_aml_uart_io_error *e)
{ (void)p; (void)fd; (void)data; (void)n; *e=HS_UART_IO_AGAIN; return -1; }
static ptrdiff_t mock_write(void *p, int fd, const uint8_t *data, size_t n, enum hs_aml_uart_io_error *e)
{
    struct mock_uart *m=p; (void)data; B(fd == 7); ++m->writes;
    if (m->bridge) {
        unsigned slot=hs_bm1362_rx_decode_long(capture,11).slot;
        B(m->bridge->chains[0].slots[slot].work == NULL);
        B(!m->bridge->chains[0].chain->jobs.slots[slot].active);
    }
    if (m->change_epoch) { m->change_epoch=0; ++m->pool->hs_stratum.clean_epoch; }
    if (m->short_error == 1) { m->short_error=2; m->bytes+=7; return 7; }
    if (m->short_error == 2) { *e=HS_UART_IO_OTHER; return -1; }
    m->bytes+=n; return (ptrdiff_t)n;
}
static int mock_wait(void *p,int fd,unsigned requested,unsigned timeout,unsigned *events,enum hs_aml_uart_io_error *e)
{ (void)p;(void)fd;(void)timeout;(void)e;*events=requested;return 1; }
static int mock_close(void *p,int fd,enum hs_aml_uart_io_error *e)
{ struct mock_uart *m=p;(void)e;B(fd == 7);++m->closes;return 0; }
static bool mock_now(void *p,uint64_t *now) { *now=((struct mock_uart *)p)->now;return true; }
static bool mock_cancel(void *p) { (void)p;return false; }
static const struct hs_aml_uart_ops uart_ops={mock_open,mock_verify,mock_control,mock_read,
    mock_write,mock_wait,mock_close,mock_now,mock_cancel};
static struct work *count_clone(void *p,const struct work *w)
{ struct work *c=hashstat_aml88_core_ops.clone(p,w);if(c)++clones;return c; }
static void count_release(void *p,struct work *w)
{ B(w != NULL);++releases;hashstat_aml88_core_ops.discard(p,w); }
static struct work *fail_clone(void *p,const struct work *w) { (void)p;(void)w;return NULL; }
static bool sink(struct thr_info *thr,struct work *w)
{
    B(thr != NULL && hashstat_work_current(w));
    B(w->hs_snapshot.actual_version == UINT32_C(0x20b44000));
    B(!memcmp(w->data+76,"\x75\x52\x03\x4c",4));
    B(fulltest(w->hash,w->target));
    ++submitted;return true;
}
static void captured_work(struct pool *p,struct work *w)
{
    uint8_t header[80];
    memset(w,0,sizeof(*w));
    B(hex2bin(header,"00000020fd55646bc162b96dfcd4f201a3f4670d1d3996bc965201000000000000000000"
        "06ddf5f08c36414b95ea54db71a0c28761a98bcf6355919e044f88725519a7cb"
        "d7685468043a02174c035275",80));
    B(hs_core_flip_header80(header,80,w->data));
    B(hex2bin(w->target,"000000000000000000000000000000000000000000000000f8ff070000000000",32));
    w->pool=p;w->stratum=true;w->job_id=strdup("bridge-capture");
    w->ntime=strdup("685468d7");w->nonce1=strdup("01020304");
    w->coinbase=strdup("fixture");w->nonce2=1;w->nonce2_len=4;
    B(w->job_id && w->ntime && w->nonce1 && w->coinbase);
    cg_wlock(&p->data_lock);
    B(hashstat_note_job_locked(p,w->job_id,UINT32_C(0x20000000),false));
    B(hashstat_capture_work_locked(p,w));
    cg_wunlock(&p->data_lock);
    B(hashstat_work_current(w));
}
static void pool_init(struct pool *p)
{
    memset(p,0,sizeof(*p));cglock_init(&p->data_lock);
    p->rpc_user="offline.bridge";
    p->hs_stratum.session_epoch=1;
    B(hashstat_apply_configure(p,true,HS_CORE_BM1362_MASK));
    p->hs_stratum.ready=p->hs_stratum.authorized=true;
}
static void make_running(struct hs_miner_lifecycle *life)
{
    struct hs_miner_lifecycle_policy policy={100,100,10,100,1};
    struct hs_miner_lifecycle_input input={0};
    B(hs_miner_lifecycle_init(&policy,life)==HS_MINER_LIFECYCLE_OK);
    input.now_ms=1000;input.command=HS_MINER_COMMAND_RUN;input.ready_flags=HS_MINER_READY_ALL;
    struct hs_miner_lifecycle_result r=hs_miner_lifecycle_step(life,&input);
    B(r.phase==HS_MINER_STARTING);
    input.command=HS_MINER_COMMAND_NONE;input.owner_event=HS_MINER_OWNER_READY;
    input.owner_generation=r.action_generation;
    B(hs_miner_lifecycle_step(life,&input).phase==HS_MINER_RUNNING);
}
static struct hs_ab_event push_capture(struct hs_aml88_bridge *bridge)
{
    struct hs_ab_event r={0};
    for(size_t i=0;i<sizeof(capture);++i) r=hs_aml88_bridge_push(bridge,0,capture[i],1000);
    return r;
}

unsigned hs_test_aml88_bridge(void)
{
    unsigned start=checks;
    const struct hs_aml88_profile *profile=hs_aml88_profile_select("BHB42801",8,"aml",3).profile;
    B(profile != NULL);
    {
        struct hs_aml88_bridge empty={0};
        struct hs_miner_lifecycle waiting;
        struct hs_miner_lifecycle_policy policy={100,100,10,100,1};
        struct hs_aml88_profile copied_profile=*profile;
        B(hs_miner_lifecycle_init(&policy,&waiting)==HS_MINER_LIFECYCLE_OK);
        B(hs_aml88_bridge_init(&empty,&copied_profile,&waiting,&hashstat_aml88_core_ops,NULL)==HS_AB_ARGUMENT);
        B(empty.self==NULL);
        B(hs_aml88_bridge_init(&empty,profile,&waiting,&hashstat_aml88_core_ops,NULL)==HS_AB_OK);
        B(hs_aml88_bridge_send(&empty,0,0,NULL,1000,1000,100).status==HS_AB_NOT_READY);
        B(waiting.phase==HS_MINER_STOPPED && clones==releases);
        B(hs_aml88_bridge_stop(&empty)==0);
    }
    for(unsigned scenario=0;scenario<9;++scenario) {
        struct pool pool;pool_init(&pool);
        struct work source;captured_work(&pool,&source);
        B(hashstat_roll_work_version(&source,UINT32_C(0x2000)));
        struct work before=source;
        struct thr_info thread={0};
        struct hs_ab_core_context context={&thread,sink};
        struct hs_ab_ops ops=hashstat_aml88_core_ops;
        ops.clone=count_clone;ops.discard=count_release;
        struct hs_miner_lifecycle life;make_running(&life);
        struct mock_uart m={0};m.now=1000;m.pool=&pool;
        struct hs_aml_uart_session uart=HS_AML_UART_SESSION_INIT;
        struct hs_aml_uart_readiness ready={true,true,true,true,true};
        B(hs_aml_uart_open(&uart,&uart_ops,&m,0,ready,100).status==HS_UART_OK);
        struct hs_aml_chain chain={0};
        B(hs_aml_chain_bind(&chain,&uart,&life,9)==HS_CHAIN_OK);
        struct hs_aml88_bridge bridge={0};
        B(hs_aml88_bridge_init(&bridge,profile,&life,&ops,&context)==HS_AB_OK);
        B(hs_aml88_bridge_init(&bridge,profile,&life,&ops,&context)==HS_AB_ARGUMENT);
        B(hs_aml88_bridge_attach(&bridge,3,&chain)==HS_AB_ARGUMENT);
        B(hs_aml88_bridge_attach(&bridge,0,&chain)==HS_AB_OK);
        m.bridge=&bridge;
        unsigned slot=hs_bm1362_rx_decode_long(capture,11).slot;
        B(hs_aml88_bridge_send(&bridge,0,slot,&source,1000,1000,100).status==HS_AB_OK);
        B(bridge.chains[0].slots[slot].work != &source);
        B(bridge.chains[0].slots[slot].work->job_id != source.job_id);
        B(m.bytes==88);
        if(scenario==0) {
            unsigned previous=submitted;
            struct hs_ab_event accepted=push_capture(&bridge);
            B(accepted.status==HS_AB_SHARE_HANDOFF);
            B(submitted==previous+1);
            for(unsigned corrupt=0;corrupt<4;++corrupt) {
                struct hs_checked_share invalid=accepted.chain.share;
                if(corrupt==0) invalid.full_version^=1;
                if(corrupt==1) invalid.digest[0]^=1;
                if(corrupt==2) invalid.version_bits|=1;
                if(corrupt==3) invalid.nonce^=1;
                B(!hashstat_aml88_core_ops.submit(&context,bridge.chains[0].slots[slot].work,&invalid));
                B(submitted==previous+1);
            }
            B(push_capture(&bridge).chain.job_status==HS_JOB_DUPLICATE);
            B(hs_aml88_bridge_flush(&bridge)==HS_AB_OK);
            B(!bridge.chains[0].slots[slot].work && !chain.jobs.slots[slot].active);
            B(push_capture(&bridge).chain.job_status==HS_JOB_EMPTY);
        } else if(scenario==1) {
            m.short_error=1;
            struct hs_ab_event r=hs_aml88_bridge_send(&bridge,0,slot,&source,1000,1000,100);
            B(r.status==HS_AB_TRANSFER_FAILED && r.chain.transfer.transferred==7);
            B(!bridge.chains[0].chain && !chain.active && !uart.active && m.bytes==95);
        } else if(scenario==2) {
            cg_wlock(&pool.data_lock);++pool.hs_stratum.clean_epoch;cg_wunlock(&pool.data_lock);
            B(push_capture(&bridge).status==HS_AB_STALE);
            B(!bridge.chains[0].slots[slot].work);
        } else if(scenario==3) {
            m.change_epoch=1;
            B(hs_aml88_bridge_send(&bridge,0,slot,&source,1000,1000,100).status==HS_AB_STALE);
            B(!bridge.chains[0].slots[slot].work && !chain.jobs.slots[slot].active);
        } else if(scenario==4) {
            life.phase=HS_MINER_DRAINING;
            B(hs_aml88_bridge_push(&bridge,0,0xaa,1000).status==HS_AB_NOT_READY);
            B(!uart.active && !bridge.chains[0].slots[slot].work);
        } else if(scenario==5) {
            bridge.next_work_tag=0;
            B(hs_aml88_bridge_send(&bridge,0,slot,&source,1000,1000,100).status==HS_AB_EXHAUSTED);
            B(!uart.active && bridge.exhausted);
        } else if(scenario==6) {
            uint64_t session=chain.jobs.session_tag;
            B(hashstat_apply_mask(&pool,HS_CORE_BM1362_MASK ^ UINT32_C(0x2000)));
            struct work newer;captured_work(&pool,&newer);
            B(hs_aml88_bridge_send(&bridge,0,slot,&newer,1000,1000,100).status==HS_AB_OK);
            B(chain.jobs.session_tag==session+1);
            clean_work(&newer);
        } else if(scenario==7) {
            chain.jobs.session_tag=UINT64_MAX;
            B(hs_aml88_bridge_flush(&bridge)==HS_AB_EXHAUSTED);
            B(!uart.active && bridge.exhausted);
        } else {
            bridge.ops.clone=fail_clone;
            B(hs_aml88_bridge_send(&bridge,0,slot,&source,1000,1000,100).status==HS_AB_WORK_REJECTED);
            B(m.bytes==88 && bridge.chains[0].slots[slot].work);
            bridge.ops.clone=count_clone;
        }
        B(!memcmp(&source,&before,sizeof(source)));
        B(hs_aml88_bridge_stop(&bridge)==0);
        B(hs_aml88_bridge_stop(&bridge)==0 && m.closes==1);
        B(clones==releases);
        clean_work(&source);
        B(pthread_mutex_destroy(&pool.data_lock.mutex)==0);
        B(pthread_rwlock_destroy(&pool.data_lock.rwlock)==0);
    }
    printf("PASS %u AML88 bridge assertions; actual core helpers, injected UART/submit only\n",checks-start);
    return checks-start;
}
#ifdef HS_AB_TEST_MAIN
int main(void) { cglock_init(&control_lock); (void)hs_test_aml88_bridge(); return 0; }
#endif
