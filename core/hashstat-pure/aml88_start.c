/* SPDX-License-Identifier: GPL-3.0-only
 * Bounded BM1362 bring-up using HashStat's protocol encoders and per-chip
 * setup. See docs/aml88-driver.md for reference revisions and limitations.
 */
#include "hs_aml88_start.h"
#include "hs_bm1362_commands.h"
#include "hs_bm1362_setup.h"
#include "hs_bm1362_pll.h"
#include "hs_bm1362_integrity.h"
#include <string.h>

struct run {
    const struct hs88_start_config *c;
    const struct hs88_start_ops *o;
    void *p;
    struct hs88_start_result r;
    uint64_t last, deadline;
    uint8_t rx[11];
    unsigned used;
};
static bool guard(struct run *r)
{
    uint64_t t;
    if (r->o->cancelled(r->p)) { r->r.status=HS88_START_CANCELLED; return false; }
    if (!r->o->now(r->p,&t) || t<r->last) { r->r.status=HS88_START_CLOCK; return false; }
    r->last=t;
    if (t>=r->deadline) { r->r.status=HS88_START_TIMEOUT; return false; }
    if (!r->o->healthy(r->p)) { r->r.status=HS88_START_INTERLOCK; return false; }
    return true;
}
static unsigned budget(struct run *r, unsigned wanted)
{
    uint64_t left=r->deadline-r->last;
    return left<wanted ? (unsigned)left : wanted;
}
static bool pause_ms(struct run *r, unsigned n)
{
    while (n) {
        unsigned part=n>10 ? 10:n;
        if (!guard(r)) return false;
        if (!r->o->sleep(r->p,part)) { r->r.status=HS88_START_IO; return false; }
        n-=part;
    }
    return guard(r);
}
static bool send_wire(struct run *r,const uint8_t *b,size_t n)
{
    if (!guard(r)) return false;
    if (r->o->trace) r->o->trace(r->p,r->r.stage,r->r.chip,b,n,false);
    if (!r->o->write(r->p,b,n,budget(r,500))) { r->r.status=HS88_START_IO; return false; }
    r->r.tx_bytes+=n;
    return guard(r);
}
static bool command(struct run *r, unsigned op, unsigned address, unsigned reg, uint32_t value)
{
    uint8_t b[11]={0x55,0xaa};
    struct hs_bm1362_command_result enc;
    switch (op) {
    case 0x41: enc=hs_bm1362_encode_register_write(address,reg,value,b+2,9);break;
    case 0x51: enc=hs_bm1362_encode_broadcast_register_write(reg,value,b+2,9);break;
    case 0x53: enc=hs_bm1362_encode_inactivate(b+2,9);break;
    case 0x40: enc=hs_bm1362_encode_address_assignment(address,b+2,9);break;
    case 0x52: enc=hs_bm1362_encode_chip_id_request(b+2,9);break;
    default: enc=hs_bm1362_encode_register_read(address,reg,b+2,9);break;
    }
    if (enc.status!=HS_BM1362_COMMAND_OK) { r->r.status=HS88_START_PROTOCOL;return false; }
    return send_wire(r,b,enc.written+2);
}
static bool clear_rx(struct run *r)
{
    r->used=0;
    if (!guard(r)) return false;
    if (!r->o->flush_rx(r->p)) {r->r.status=HS88_START_IO;return false;}
    return true;
}
static bool feed(struct run *r,uint8_t b,uint8_t frame[11])
{
    if (!r->used) {if(b==0xaa)r->rx[r->used++]=b;return false;}
    if(r->used==1 && b!=0x55) {r->used=b==0xaa?1:0;return false;}
    r->rx[r->used++]=b;
    if(r->used!=11)return false;
    struct hs_bm1362_integrity in=hs_bm1362_inspect_integrity(r->rx,11);
    if(in.status==HS_BM1362_INTEGRITY_OK) {
        memcpy(frame,r->rx,11);r->used=0;
        return in.kind==HS_BM1362_RESPONSE_REGISTER;
    }
    if(!in.crc_valid)++r->r.crc_errors;
    for(unsigned i=1;i<10;++i)if(r->rx[i]==0xaa && r->rx[i+1]==0x55){
        r->used=11-i;memmove(r->rx,r->rx+i,r->used);return false;
    }
    r->used=r->rx[10]==0xaa?1:0;if(r->used)r->rx[0]=0xaa;
    return false;
}
static uint32_t value32(const uint8_t f[11])
{return (uint32_t)f[2]<<24|(uint32_t)f[3]<<16|(uint32_t)f[4]<<8|f[5];}
static bool chip_id(struct run *r,const uint8_t f[11])
{return f[r->c->chip_id_offset]==0x13 && f[r->c->chip_id_offset+1]==0x62;}
/* Exactly one matching response for an addressed read. Unrelated frames are
 * ignored under a byte/time budget; no serial timeout is treated as success. */
static bool read_reg(struct run *r,unsigned a,unsigned reg,uint32_t *value,bool identity)
{
    uint64_t end;
    unsigned bytes=0, attempts=0;
    uint8_t b[64],f[11];
    if(!clear_rx(r)||!command(r,0x42,a,reg,0))return false;
    end=r->last+budget(r,r->c->reply_timeout_ms);
    while(r->last<end && bytes<8192 && ++attempts<=4096) {
        if(!guard(r))return false;
        if(r->last>=end)break;
        unsigned t=(unsigned)(end-r->last);if(t>20)t=20;
        int n=r->o->read(r->p,b,sizeof(b),t);
        if(n<0 || n>(int)sizeof(b)){r->r.status=HS88_START_IO;return false;}
        if(r->o->trace && n)r->o->trace(r->p,r->r.stage,r->r.chip,b,(size_t)n,true);
        bytes+=(unsigned)n;r->r.rx_bytes+=(unsigned)n;
        for(int i=0;i<n;++i)if(feed(r,b[i],f) && f[6]==a && f[7]==reg){
            *value=value32(f);
            if(identity && !chip_id(r,f)){r->r.status=HS88_START_ID;r->r.observed=*value;return false;}
            return guard(r);
        }
        if(!guard(r))return false;
    }
    r->r.status=HS88_START_READBACK;return false;
}
static bool enumerate(struct run *r)
{
    uint8_t b[128],f[11];unsigned bytes=0, attempts=0;
    if(!clear_rx(r)||!command(r,0x52,0,0,0))return false;
    uint64_t end=r->last+budget(r,2000);
    while(r->last<end && bytes<16384 && ++attempts<=4096){
        if(!guard(r))return false;
        if(r->last>=end)break;
        unsigned wait=(unsigned)(end-r->last);if(wait>20)wait=20;
        int n=r->o->read(r->p,b,sizeof(b),wait);
        if(n<0 || n>(int)sizeof(b)){r->r.status=HS88_START_IO;return false;}
        bytes+=(unsigned)n;r->r.rx_bytes+=(unsigned)n;
        if(r->o->trace && n)r->o->trace(r->p,r->r.stage,r->r.chip,b,(size_t)n,true);
        for(int i=0;i<n;++i)if(feed(r,b[i],f) && f[7]==0){
            if(!chip_id(r,f)){r->r.status=HS88_START_ID;r->r.observed=value32(f);return false;}
            if(++r->r.discovered>88){r->r.status=HS88_START_COUNT;return false;}
        }
        if(!guard(r))return false;
    }
    if(attempts>4096 || bytes>=16384){r->r.status=HS88_START_PROTOCOL;return false;}
    if(r->r.discovered!=88){r->r.status=HS88_START_COUNT;return false;}
    return true;
}
static bool setup_chip(struct run *r,unsigned a)
{
    uint32_t a8,reg18;
    if(!read_reg(r,a,0xa8,&a8,false)||!read_reg(r,a,0x18,&reg18,false))return false;
    struct hs_bm1362_setup s={0};
    struct hs_bm1362_setup_config c={0};
    c.attempt_tag=1;c.chip_address=a;c.chain_cached_a8=a8;c.chain_cached_18=reg18;c.write_timeout_ms=500;
    if(hs_bm1362_setup_init(&s,&c,r->last)!=HS_SETUP_PENDING){r->r.status=HS88_START_PROTOCOL;return false;}
    for(unsigned steps=0;steps<128;++steps){
        if(!guard(r))return false;
        struct hs_bm1362_setup_result out=hs_bm1362_setup_next(&s,r->last);
        if(out.status==HS_SETUP_TX_COMPLETE)return true;
        if(out.status==HS_SETUP_WAIT){if(!pause_ms(r,out.remaining_ms))return false;continue;}
        if(out.status!=HS_SETUP_WRITE){r->r.status=HS88_START_PROTOCOL;return false;}
        if(!send_wire(r,out.frame,out.frame_bytes))return false;
        out=hs_bm1362_setup_ack(&s,r->last,1,out.write_id,HS_SETUP_TRANSFER_COMPLETE,out.frame_bytes,0);
        if(out.status==HS_SETUP_FAILED){r->r.status=HS88_START_PROTOCOL;return false;}
    }
    r->r.status=HS88_START_PROTOCOL;return false;
}
struct hs88_start_result hs88_start_chain(const struct hs88_start_config *c,
                                          const struct hs88_start_ops *o,void *p)
{
    struct run r={.c=c,.o=o,.p=p,.r={.status=HS88_START_ARGUMENT,.stage="validate"}};
    struct hs_bm1362_pll_result pll;
    if(!c||!o||!o->prepare||!o->reset||!o->healthy||!o->cancelled||!o->now||!o->sleep||
       !o->flush_rx||!o->write||!o->read||!o->off || c->frequency_mhz<50||c->frequency_mhz>200||
       c->timeout_ms<1000||c->timeout_ms>120000||c->reply_timeout_ms<20||c->reply_timeout_ms>2000||
       (c->chip_id_offset!=2 && c->chip_id_offset!=3))return r.r;
    if(hs_bm1362_pll_for_frequency((int32_t)c->frequency_mhz,&pll)!=HS_BM1362_PLL_OK)return r.r;
    r.r.pll=pll.register_value;
    if(!o->now(p,&r.last)||UINT64_MAX-r.last<c->timeout_ms){r.r.status=HS88_START_CLOCK;return r.r;}
    r.deadline=r.last+c->timeout_ms;
    r.r.stage="prepare";r.r.status=HS88_START_INTERLOCK;
    if(!o->prepare(p)||!guard(&r))goto fail;
    r.r.stage="reset";r.r.status=HS88_START_IO;
    if(!o->reset(p,true)||!pause_ms(&r,100)||!o->reset(p,false)||!pause_ms(&r,100))goto fail;
    r.r.stage="long-reply";
    if(!command(&r,0x51,0,0xa4,0x9000ffff)||!pause_ms(&r,100))goto fail;
    r.r.stage="enumerate";
    if(!enumerate(&r))goto fail;
    r.r.stage="inactivate";
    for(unsigned i=0;i<3;++i)if(!command(&r,0x53,0,0,0)||!pause_ms(&r,100))goto fail;
    r.r.stage="assign";
    for(unsigned i=0;i<88;++i){r.r.chip=i;if(!command(&r,0x40,2*i,0,0)||!pause_ms(&r,10))goto fail;++r.r.assigned;}
    r.r.stage="verify-addresses";
    for(unsigned i=0;i<88;++i){uint32_t v;r.r.chip=i;if(!read_reg(&r,2*i,0,&v,true))goto fail;}
    r.r.stage="chain-setup";
    if(!command(&r,0x51,0,0x54,3)||!pause_ms(&r,50)||
       !command(&r,0x51,0,0x58,0x00011111)||!pause_ms(&r,100)||
       !command(&r,0x51,0,0x14,0xf0)||!pause_ms(&r,20))goto fail;
    r.r.stage="chip-setup";
    for(unsigned i=0;i<88;++i){r.r.chip=i;if(!setup_chip(&r,i*2))goto fail;++r.r.configured;}
    r.r.stage="pll";
    if(!command(&r,0x51,0,0x70,0x0f0f0f00)||!pause_ms(&r,10)||
       !command(&r,0x51,0,0x08,pll.register_value)||!pause_ms(&r,100)||
       !command(&r,0x51,0,0x10,0x000012c9)||!pause_ms(&r,10))goto fail;
    r.r.stage="verify-pll";
    for(unsigned i=0;i<88;++i){uint32_t v;r.r.chip=i;if(!read_reg(&r,2*i,0x08,&v,false))goto fail;
        r.r.expected=pll.register_value;r.r.observed=v;
        if((v&0x7fffffff)!=(pll.register_value&0x7fffffff)){r.r.status=HS88_START_READBACK;goto fail;}}
    r.r.stage="verify-ticket";
    for(unsigned i=0;i<88;++i){uint32_t v;r.r.chip=i;if(!read_reg(&r,2*i,0x14,&v,false))goto fail;
        r.r.expected=0xf0;r.r.observed=v;if(v!=0xf0){r.r.status=HS88_START_READBACK;goto fail;}}
    if(!clear_rx(&r))goto fail;
    r.r.stage="ready-for-work";r.r.status=HS88_START_OK;return r.r;
fail:
    r.r.reset_asserted_on_error=o->reset(p,true);
    r.r.power_off_on_error=o->off(p);
    return r.r;
}
const char *hs88_start_status_name(enum hs88_start_status s)
{
    static const char *const names[]={"ok","argument","cancelled","clock","timeout","interlock","io","chip-count","chip-id","readback","protocol"};
    return (unsigned)s<sizeof(names)/sizeof(names[0])?names[s]:"invalid";
}
