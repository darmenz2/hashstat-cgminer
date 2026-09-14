/* SPDX-License-Identifier: GPL-3.0-only
 * One-chain BM1362 bench driver. Hardware access is opt-in and failure-latched.
 * See docs/aml88-driver.md before connecting power.
 */
#include "config.h"
#include "miner.h"
#include "hashstat-core.h"
#include "hashstat-aml88-core.h"
#include "hashstat-aml88-linux.h"
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#include <limits.h>

struct hs88_device {
    struct hs88_linux_config config;
    const struct hs_aml88_profile *profile;
    struct hs88_linux *hw;
    struct hs_miner_lifecycle life;
    struct hs_aml_chain chain;
    struct hs_aml88_bridge bridge;
    struct hs_ab_core_context core;
    atomic_bool flush;
    bool stopped, started;
    unsigned slot;
    uint64_t last_send;
    double hash_credit;
};
static uint64_t hs88_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000U + (unsigned)t.tv_nsec / 1000000U;
}
/* The bridge owns cloned work; get_queued() work is released immediately
 * after a successful clone/send, never retained in two ownership systems. */
static struct work *clone_job(void *p,const struct work *w)
{struct hs88_device *d=p;return hashstat_aml88_core_ops.clone(&d->core,w);}
static void discard_job(void *p,struct work *w)
{struct hs88_device *d=p;hashstat_aml88_core_ops.discard(&d->core,w);}
static bool export_job(void *p,struct work *w,struct hs_ab_export *out)
{
    struct hs88_device *d=p;
    return hashstat_aml88_core_ops.export_work(&d->core,w,out) &&
        out->version_mask==UINT32_C(0x1fffe000);
}
static bool current_job(void *p,struct work *w)
{struct hs88_device *d=p;return hashstat_aml88_core_ops.current(&d->core,w);}
static bool submit_job(void *p,const struct work *w,const struct hs_checked_share *s)
{
    struct hs88_device *d=p;
    bool ok=hashstat_aml88_core_ops.submit(&d->core,w,s);
    /* Pool-target-weighted, locally verified shares only. Raw nonce packets
     * are not evidence of hashes. This estimator is noisy at high pool diff. */
    if(ok && isfinite(w->work_difficulty) && w->work_difficulty>0)
        d->hash_credit+=w->work_difficulty*4294967296.0;
    return ok;
}
static const struct hs_ab_ops bridge_ops={clone_job,discard_job,export_job,current_job,submit_job};
static void stop_device(struct hs88_device *d)
{
    if(!d||d->stopped)return;
    d->stopped=true;
    struct hs_miner_lifecycle_input in={.now_ms=hs88_now(),.command=HS_MINER_COMMAND_SHUTDOWN};
    if(d->life.abi==HS_MINER_LIFECYCLE_ABI)(void)hs_miner_lifecycle_step(&d->life,&in);
    /* Power off first, before potentially blocking userspace work cleanup. */
    bool off_ok=hs88_linux_shutdown(d->hw);
    if(d->bridge.self)(void)hs_aml88_bridge_stop(&d->bridge);
    if(!off_ok)applog(LOG_ERR,"HS88: shutdown did not confirm every cleanup operation");
    if(d->life.abi==HS_MINER_LIFECYCLE_ABI && off_ok) {
        in.now_ms=hs88_now();in.command=HS_MINER_COMMAND_NONE;
        in.owner_event=HS_MINER_OWNER_DRAINED;in.owner_generation=d->life.attempt_generation;
        (void)hs_miner_lifecycle_step(&d->life,&in);
    }
}
static bool init_thread(struct thr_info *thr)
{
    struct hs88_device *d=thr->cgpu->device_data;
    const struct hs_miner_lifecycle_policy policy={120000,5000,1000,1000,0};
    d->core.thread=thr;
    if(hs_miner_lifecycle_init(&policy,&d->life)!=HS_MINER_LIFECYCLE_OK)goto failed;
    const struct hs88_start_config c={d->config.frequency_mhz,120000,500,d->config.chip_id_offset};
    struct hs88_start_result r=hs88_start_chain(&c,hs88_linux_start_ops(),d->hw);
    if(r.status!=HS88_START_OK) {
        applog(LOG_ERR,"HS88: start %s at %s chip=%u found=%u assigned=%u configured=%u expected=%08x observed=%08x reset=%u off=%u (%s)",
            hs88_start_status_name(r.status),r.stage,r.chip,r.discovered,r.assigned,r.configured,
            r.expected,r.observed,r.reset_asserted_on_error,r.power_off_on_error,hs88_linux_error(d->hw));
        goto failed;
    }
    if(!hs88_linux_alive(d->hw))goto failed;
    /* READY is emitted only after the physical startup has returned success.
     * The supervisory state never substitutes for the hardware checks. */
    struct hs_miner_lifecycle_input in={.now_ms=hs88_now(),.command=HS_MINER_COMMAND_RUN,.ready_flags=HS_MINER_READY_ALL};
    struct hs_miner_lifecycle_result lr=hs_miner_lifecycle_step(&d->life,&in);
    if(lr.status!=HS_MINER_LIFECYCLE_OK||lr.phase!=HS_MINER_STARTING)goto failed;
    in.command=HS_MINER_COMMAND_NONE;in.owner_event=HS_MINER_OWNER_READY;
    in.owner_generation=d->life.attempt_generation;in.now_ms=hs88_now();
    lr=hs_miner_lifecycle_step(&d->life,&in);
    if(lr.status!=HS_MINER_LIFECYCLE_OK||!lr.work_permitted)goto failed;
    if(hs_aml_chain_bind(&d->chain,hs88_linux_uart(d->hw),&d->life,1)!=HS_CHAIN_OK ||
       hs_aml88_bridge_init(&d->bridge,d->profile,&d->life,&bridge_ops,d)!=HS_AB_OK ||
       hs_aml88_bridge_attach(&d->bridge,d->config.chain,&d->chain)!=HS_AB_OK)goto failed;
    d->started=true;hs88_linux_progress(d->hw);
    applog(LOG_WARNING,"HS88: experimental one-chain runtime started (%s, chain %u, %u MHz); accepted shares still required to prove mining",
        d->config.model,d->config.chain,d->config.frequency_mhz);
    return true;
failed:
    stop_device(d);return false;
}
static bool queue_work(struct cgpu_info *cgpu)
{
    struct hs88_device *d=cgpu->device_data;
    if(d->stopped||!hs88_linux_alive(d->hw)){stop_device(d);return true;}
    hs88_linux_progress(d->hw);
    if(atomic_exchange(&d->flush,false)) {
        if(hs_aml88_bridge_flush(&d->bridge)!=HS_AB_OK){stop_device(d);return true;}
        d->last_send=0;
    }
    uint64_t now=hs88_now();
    if(d->last_send && now>=d->last_send && now-d->last_send<d->config.work_interval_ms)return true;
    struct work *w=get_queued(cgpu);
    if(!w)return true;
    /* Refuse unsupported version-rolling negotiation before hardware writes. */
    if(!w->hs_snapshot.rolling || w->hs_snapshot.mask!=UINT32_C(0x1fffe000)) {
        applog(LOG_ERR,"HS88: pool must negotiate version mask 1fffe000 for this bench implementation");
        work_completed(cgpu,w);stop_device(d);return true;
    }
    struct hs_ab_event ev=hs_aml88_bridge_send(&d->bridge,d->config.chain,d->slot,w,now,5000,500);
    work_completed(cgpu,w);
    if(ev.status==HS_AB_OK){d->last_send=now;d->slot=(d->slot+3U)%HS_JOB_CACHE_SLOTS;}
    else if(ev.status!=HS_AB_STALE){applog(LOG_ERR,"HS88: work send failed (%u)",(unsigned)ev.status);stop_device(d);}
    return true;
}
static int64_t scan_work(struct thr_info *thr)
{
    struct hs88_device *d=thr->cgpu->device_data;uint8_t bytes[512];
    if(d->stopped||!hs88_linux_alive(d->hw)){stop_device(d);return -1;}
    hs88_linux_progress(d->hw);
    int n=hs88_linux_read(d->hw,bytes,sizeof(bytes),20);
    if(n<0){applog(LOG_ERR,"HS88: receive or safety failure");stop_device(d);return -1;}
    uint64_t now=hs88_now();
    for(int i=0;i<n;++i){struct hs_ab_event ev=hs_aml88_bridge_push(&d->bridge,d->config.chain,bytes[i],now);
        if(ev.status==HS_AB_NOT_READY||ev.status==HS_AB_EXHAUSTED){stop_device(d);return -1;}}
    /* Cap credit before the integer conversion; no out-of-range float casts. */
    double credit=d->hash_credit;
    if(!isfinite(credit)||credit<0){stop_device(d);return -1;}
    if(credit>1.0e18)credit=1.0e18;
    int64_t hashes=(int64_t)credit;d->hash_credit-=(double)hashes;return hashes;
}
static void flush_work(struct cgpu_info *cgpu)
{struct hs88_device *d=cgpu->device_data;if(d)atomic_store(&d->flush,true);}
static void shutdown_thread(struct thr_info *thr)
{stop_device(thr->cgpu->device_data);}
extern struct device_drv hashstat_aml88_drv;
static void hashstat_aml88_detect(bool hotplug)
{
    static bool registered;
    const char *path=getenv("HASHSTAT_AML88_CONFIG");
    if(hotplug||registered)return;
    if(!path||!*path){applog(LOG_INFO,"HS88: disabled; explicit reviewed HASHSTAT_AML88_CONFIG required");return;}
    struct hs88_linux_config config;char error[192];
    if(!hs88_config_load(path,&config,error,sizeof(error))){applog(LOG_ERR,"HS88: %s",error);return;}
    struct hs_aml88_profile_result p=hs_aml88_profile_select(config.model,strlen(config.model),"aml",3);
    if(p.status!=HS_AML88_PROFILE_OK){applog(LOG_ERR,"HS88: unsupported exact board profile");return;}
    struct cgpu_info *cgpu=calloc(1,sizeof(*cgpu));
    struct hs88_device *d=calloc(1,sizeof(*d));
    if(!cgpu||!d){free(cgpu);free(d);return;}
    d->config=config;d->profile=p.profile;atomic_init(&d->flush,false);
    d->hw=hs88_linux_new(&config);
    if(!d->hw){free(d);free(cgpu);return;}
    cgpu->drv=&hashstat_aml88_drv;cgpu->deven=DEV_ENABLED;cgpu->threads=1;cgpu->device_data=d;
    cgpu->device_path=strdup(hs_aml_uart_path(config.chain));
    if(!cgpu->device_path||!add_cgpu(cgpu)){hs88_linux_free(d->hw);free(cgpu->device_path);free(d);free(cgpu);return;}
    registered=true;
}
struct device_drv hashstat_aml88_drv={
    .drv_id=DRIVER_hashstat_aml88,.dname="hashstat-aml88-bench",.name="HS88",
    .drv_detect=hashstat_aml88_detect,.thread_init=init_thread,.hash_work=hash_queued_work,
    .queue_full=queue_work,.scanwork=scan_work,.flush_work=flush_work,.update_work=flush_work,
    .thread_shutdown=shutdown_thread,.max_diff=1.0e12,.min_diff=1.0,
};
