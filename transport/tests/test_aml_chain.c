/* SPDX-License-Identifier: GPL-3.0-only
 * Reuse the deterministic UART mock and rerun all original UART regressions.
 * All hardware callbacks are injected; no actual device is opened.
 * Captured nonce/header: Mujina GPL-3.0-or-later,
 * 019d1b0916457bb7ecd7cb7e93fee63c0329f9a2; see CAPTURE-INTEROPERABILITY.md.
 */
#ifdef NDEBUG
#error "This executable is a test: assertions must remain enabled"
#endif
#define main hs_uart_regressions
#include "test_aml_uart.c"
#undef main
#include "hs_aml_chain.h"
#include "hs_btm_work_wire.h"

static unsigned chain_checks;
#define C(x) do { ++chain_checks; assert(x); } while (0)
static const uint8_t capture[11] = {0xaa,0x55,0x4c,0x03,0x52,0x75,0x0c,0xd2,0x05,0xa2,0x9c};
static unsigned digit(char c) { return c <= '9' ? (unsigned)(c-'0') : (unsigned)(c-'a')+10U; }
static void unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)((digit(s[2*i])<<4)|digit(s[2*i+1]));
}
static struct hs_job_snapshot captured_job(void)
{
    struct hs_job_snapshot j = {0};
    unhex("00000020fd55646bc162b96dfcd4f201a3f4670d1d3996bc965201000000000000000000"
          "06ddf5f08c36414b95ea54db71a0c28761a98bcf6355919e044f88725519a7cb"
          "d7685468043a02174c035275", j.header, 80);
    unhex("000000000000000000000000000000000000000000000000f8ff070000000000", j.share_target_le, 32);
    j.session_tag=9; j.job_tag=10; j.work_tag=11; j.issued_ms=1000;
    j.max_age_ms=1000; j.version_mask=HS_BM1362_RX_VERSION_BITS_MASK;
    return j;
}
static struct hs_miner_lifecycle running(void)
{
    struct hs_miner_lifecycle l;
    struct hs_miner_lifecycle_policy p={100,100,10,100,1};
    struct hs_miner_lifecycle_input i={0};
    C(hs_miner_lifecycle_init(&p,&l)==HS_MINER_LIFECYCLE_OK);
    i.now_ms=1000; i.command=HS_MINER_COMMAND_RUN; i.ready_flags=HS_MINER_READY_ALL;
    struct hs_miner_lifecycle_result r=hs_miner_lifecycle_step(&l,&i);
    C(r.phase==HS_MINER_STARTING && !r.work_permitted);
    i.command=HS_MINER_COMMAND_NONE; i.owner_event=HS_MINER_OWNER_READY;
    i.owner_generation=r.action_generation;
    r=hs_miner_lifecycle_step(&l,&i);
    C(r.phase==HS_MINER_RUNNING && r.work_permitted);
    return l;
}
static struct hs_aml_chain_event push(struct hs_aml_chain *c,const uint8_t *data,size_t n,uint64_t now)
{
    struct hs_aml_chain_event e={0};
    for(size_t i=0;i<n;++i) e=hs_aml_chain_push(c,data[i],now);
    return e;
}
static void fix_crc(uint8_t frame[11],uint8_t type)
{
    for(unsigned v=0;v<32;++v) {
        frame[10]=(uint8_t)(type|v);
        if(hs_bm1362_inspect_integrity(frame,11).crc_valid) return;
    }
    C(0);
}
#define BENCH() \
    struct mock m; fixture(&m); \
    struct hs_aml_uart_session uart=HS_AML_UART_SESSION_INIT; opened(&m,&uart); \
    struct hs_miner_lifecycle life=running(); \
    struct hs_aml_chain chain={0}; \
    C(hs_aml_chain_bind(&chain,&uart,&life,9)==HS_CHAIN_OK); \
    struct hs_job_snapshot job=captured_job(); \
    unsigned slot=hs_bm1362_rx_decode_long(capture,11).slot

static struct hs_aml_chain *observed_chain;
static unsigned observed_slot;
static ptrdiff_t observe_write(void *ctx,int fd,const uint8_t *bytes,size_t n,
                              enum hs_aml_uart_io_error *error)
{
    C(observed_chain->jobs.slots[observed_slot].active==0);
    return mock_write(ctx,fd,bytes,n,error);
}
static void test_end_to_end(void)
{
    BENCH();
    C(hs_bm1362_inspect_integrity(capture,11).status==HS_BM1362_INTEGRITY_OK);
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    C(m.written_count==88 && chain.jobs.slots[slot].active==1);
    uint8_t prefix[76], wire[88];
    C(hs_btm_work_ring_from_header80(job.header,80,prefix,76).status==HS_WORK_WIRE_OK);
    C(hs_btm_work_wire_pack(prefix,76,slot,wire,88).status==HS_WORK_WIRE_OK);
    C(memcmp(m.written,wire,88)==0 && wire[3]==0x36);
    struct hs_aml_chain_event e=push(&chain,capture,11,1000);
    C(e.status==HS_CHAIN_SHARE && e.share.nonce==UINT32_C(0x7552034c));
    C(e.share.full_version==UINT32_C(0x20b44000) && e.share.work_tag==11);
    e=push(&chain,capture,11,1000);
    C(e.status==HS_CHAIN_SHARE_REJECTED && e.job_status==HS_JOB_DUPLICATE);
    observed_chain=&chain; observed_slot=slot; uart.ops.write=observe_write;
    m.actions[0]=(struct io_action){1,HS_UART_IO_NONE};m.action_count=1;
    job.work_tag=12;
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    C(m.written_count==176 && chain.jobs.slots[slot].work.work_tag==12);
    C(push(&chain,capture,11,1000).status==HS_CHAIN_SHARE);
    observed_chain=NULL;
    C(hs_aml_chain_stop(&chain)==0 && m.close_calls==1 && !uart.active);
    C(hs_aml_chain_stop(&chain)==0 && m.close_calls==1);
}
static void test_rejections(void)
{
    BENCH();
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    struct hs_job_snapshot bad=job;
    bad.session_tag=8;
    C(hs_aml_chain_send(&chain,slot,&bad,1000,100).job_status==HS_JOB_WRONG_SESSION);
    bad=job;memset(bad.share_target_le,0,32);
    C(hs_aml_chain_send(&chain,slot,&bad,1000,100).job_status==HS_JOB_INVALID);
    bad=job;bad.version_mask=UINT32_MAX;
    C(hs_aml_chain_send(&chain,slot,&bad,1000,100).job_status==HS_JOB_VERSION_REJECTED);
    bad=job;bad.header[2]=1;
    C(hs_aml_chain_send(&chain,slot,&bad,1000,100).job_status==HS_JOB_VERSION_REJECTED);
    bad=job;bad.issued_ms=1001;
    C(hs_aml_chain_send(&chain,slot,&bad,1000,100).job_status==HS_JOB_STALE);
    C(hs_aml_chain_send(&chain,32,&job,1000,100).status==HS_CHAIN_ARGUMENT);
    C(hs_aml_chain_send(&chain,slot,NULL,1000,100).status==HS_CHAIN_ARGUMENT);
    C(hs_aml_chain_send(&chain,slot,&job,1000,0).status==HS_CHAIN_ARGUMENT);
    C(m.written_count==88 && chain.jobs.slots[slot].work.work_tag==11);
    C(hs_aml_chain_stop(&chain)==0);
}
static void test_stream(void)
{
    BENCH();
    for(unsigned bit=16;bit<88;++bit) {
        C(hs_aml_chain_reset_jobs(&chain,9)==HS_CHAIN_OK);
        C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
        uint8_t noise[11];memcpy(noise,capture,11);
        noise[bit/8]^=(uint8_t)(1U<<(bit%8));
        C(push(&chain,noise,11,1000).status==HS_CHAIN_BAD_CRC);
        C(push(&chain,capture,11,1000).status==HS_CHAIN_SHARE);
    }
    C(hs_aml_chain_reset_jobs(&chain,9)==HS_CHAIN_OK);
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    const uint8_t overlap[]={0,0xaa,0xaa,0x55,0x11,0x22};
    (void)push(&chain,overlap,sizeof(overlap),1000);
    C(push(&chain,capture,11,1000).status==HS_CHAIN_SHARE);
    uint8_t control[11]={0xaa,0x55};fix_crc(control,0);
    struct hs_aml_chain_event e=push(&chain,control,11,1000);
    C(e.status==HS_CHAIN_REGISTER && memcmp(e.register_frame,control,11)==0);
    fix_crc(control,0x20);
    C(push(&chain,control,11,1000).status==HS_CHAIN_BAD_TYPE);
    const uint8_t wrong_chip[11]={0xaa,0x55,0x81,0xc9,0x77,0xd0,0x00,0xf2,0x7c,0x3e,0x89};
    C(push(&chain,wrong_chip,11,1000).status==HS_CHAIN_BAD_CHIP);
    C(hs_aml_chain_reset_jobs(&chain,10)==HS_CHAIN_OK);
    e=push(&chain,capture,11,1000);
    C(e.status==HS_CHAIN_SHARE_REJECTED && e.job_status==HS_JOB_EMPTY);
    C(hs_aml_chain_reset_jobs(&chain,9)==HS_CHAIN_ARGUMENT);
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).job_status==HS_JOB_WRONG_SESSION);
    job.session_tag=10;
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    C(push(&chain,capture,11,2000).job_status==HS_JOB_STALE);
    C(hs_aml_chain_stop(&chain)==0);
}
static void test_partial_write_and_cleanup(void)
{
    BENCH();
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    m.actions[0]=(struct io_action){7,HS_UART_IO_NONE};
    m.actions[1]=(struct io_action){-1,HS_UART_IO_OTHER};m.action_count=2;
    m.close_fail=true;m.restore_fail=true;
    struct hs_aml_chain_event e=hs_aml_chain_send(&chain,slot,&job,1000,100);
    C(e.status==HS_CHAIN_TRANSPORT_FAILED && e.transfer.transferred==7);
    C(e.transfer.cleanup_errors==(HS_UART_CLOSE_FAILED|HS_UART_RESTORE_FAILED));
    C(chain.cleanup_errors==e.transfer.cleanup_errors && !chain.active && !uart.active);
    for(unsigned i=0;i<HS_JOB_CACHE_SLOTS;++i) C(chain.jobs.slots[i].active==0);
    C(m.close_calls==1);
    e=hs_aml_chain_send(&chain,slot,&job,1000,100);
    C(e.status==HS_CHAIN_CLOSED && e.transfer.cleanup_errors==chain.cleanup_errors);
    C(m.written_count==95 && m.close_calls==1);
}
static void test_gate_and_clock(void)
{
    BENCH();
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    struct hs_aml_chain copied=chain;
    C(hs_aml_chain_push(&copied,0xaa,1000).status==HS_CHAIN_ARGUMENT);
    C(hs_aml_chain_stop(&copied)==0 && uart.active);
    life.phase=HS_MINER_DRAINING;m.close_fail=true;
    struct hs_aml_chain_event e=hs_aml_chain_push(&chain,0xaa,1000);
    C(e.status==HS_CHAIN_NOT_READY && e.transfer.cleanup_errors==HS_UART_CLOSE_FAILED);
    C(!chain.active && !uart.active && m.close_calls==1);
    struct hs_aml_chain zero={0};
    C(hs_aml_chain_bind(&zero,&uart,&life,9)==HS_CHAIN_NOT_READY);
    C(hs_job_snapshot_validate(NULL,9)==HS_JOB_INVALID);
    C(hs_job_snapshot_validate(&job,0)==HS_JOB_INVALID);
}
static void test_backward_clock(void)
{
    BENCH();
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    C(hs_aml_chain_push(&chain,0xaa,999).status==HS_CHAIN_CLOCK_ERROR);
    C(!uart.active && !chain.active && m.close_calls==1);
}
static void test_generation_latch(void)
{
    BENCH();
    m.actions[0]=(struct io_action){7,HS_UART_IO_NONE};
    m.actions[1]=(struct io_action){-1,HS_UART_IO_OTHER};m.action_count=2;
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_TRANSPORT_FAILED);
    C(chain.cleanup_errors==0 && !uart.active);
    fixture(&m);opened(&m,&uart);
    C(hs_aml_chain_bind(&chain,&uart,&life,10)==HS_CHAIN_NOT_READY);
    C(m.written_count==0);
    struct hs_miner_lifecycle_input i={0};i.now_ms=1000;
    i.command=HS_MINER_COMMAND_STOP;i.ready_flags=HS_MINER_READY_ALL;
    C(hs_miner_lifecycle_step(&life,&i).phase==HS_MINER_DRAINING);
    i.command=HS_MINER_COMMAND_NONE;i.owner_event=HS_MINER_OWNER_DRAINED;
    i.owner_generation=life.attempt_generation;
    C(hs_miner_lifecycle_step(&life,&i).phase==HS_MINER_STOPPED);
    i.owner_event=HS_MINER_OWNER_NONE;i.command=HS_MINER_COMMAND_RUN;
    C(hs_miner_lifecycle_step(&life,&i).phase==HS_MINER_STARTING);
    C(hs_aml_chain_bind(&chain,&uart,&life,10)==HS_CHAIN_NOT_READY);
    i.owner_event=HS_MINER_OWNER_READY;i.owner_generation=life.attempt_generation;
    i.command=HS_MINER_COMMAND_NONE;
    C(hs_miner_lifecycle_step(&life,&i).phase==HS_MINER_RUNNING);
    C(hs_aml_chain_bind(&chain,&uart,&life,10)==HS_CHAIN_OK);
    job.session_tag=10;
    C(hs_aml_chain_send(&chain,slot,&job,1000,100).status==HS_CHAIN_OK);
    C(hs_aml_chain_stop(&chain)==0);
}
static void test_missing_hardware(void)
{
    struct mock m;fixture(&m);
    struct hs_aml_uart_session uart=HS_AML_UART_SESSION_INIT;opened(&m,&uart);
    struct hs_miner_lifecycle life;
    struct hs_miner_lifecycle_policy policy={100,100,10,100,1};
    C(hs_miner_lifecycle_init(&policy,&life)==HS_MINER_LIFECYCLE_OK);
    struct hs_miner_lifecycle_input i={0};i.now_ms=1000;i.command=HS_MINER_COMMAND_RUN;
    i.ready_flags=HS_MINER_READY_CONTROLLER;
    C(hs_miner_lifecycle_step(&life,&i).phase==HS_MINER_WAITING_HARDWARE);
    struct hs_aml_chain chain={0};
    C(hs_aml_chain_bind(&chain,&uart,&life,9)==HS_CHAIN_NOT_READY);
    C(m.written_count==0);
    closed(&m,&uart);
}
int main(void)
{
    C(hs_uart_regressions()==0);
    test_end_to_end();test_rejections();test_stream();test_partial_write_and_cleanup();
    test_gate_and_clock();test_backward_clock();test_generation_latch();test_missing_hardware();
    printf("AML chain integration PASS: %u checks; injected UART; no hardware\n",chain_checks);
    return 0;
}
