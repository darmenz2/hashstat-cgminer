/* SPDX-License-Identifier: GPL-3.0-only
 * Protocol emulator, not a model of electrical behaviour or silicon timing.
 */
#include "hs_aml88_start.h"
#include "hs_bm1362_commands.h"
#include "hs_bm1362_integrity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define C(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);assert(x);} } while(0)
struct mock {
    uint64_t time;
    unsigned chips,id_offset,chunk,assign,writes,reads,write_fail,cancel_at,health_at;
    unsigned off_calls,reset_calls,prepared;
    bool prepare_fail,reset_fail,off_fail,flush_fail,read_fail,bad_pll,bad_ticket;
    bool bad_id,bad_address,corrupt,noise,silent,frozen,reverse,oversize;
    uint32_t registers[88][256];
    uint8_t buffer[4096];size_t head,tail;
};
static void response(struct mock *m,unsigned addr,unsigned reg,uint32_t value)
{
    uint8_t f[11]={0xaa,0x55,(uint8_t)(value>>24),(uint8_t)(value>>16),(uint8_t)(value>>8),(uint8_t)value,(uint8_t)addr,(uint8_t)reg,0,0,0};
    if(!reg){f[2]=f[3]=f[4]=f[5]=0;f[m->id_offset]=0x13;f[m->id_offset+1]=m->bad_id?0x70:0x62;}
    if(m->bad_address && addr==174)f[6]=172;
    bool found=false;for(unsigned c=0;c<32;++c){f[10]=(uint8_t)c;if(hs_bm1362_inspect_integrity(f,11).status==HS_BM1362_INTEGRITY_OK){found=true;break;}}
    C(found);
    if(m->corrupt)f[4]^=1;
    C(m->tail+11+(m->noise?3:0)<=sizeof(m->buffer));
    if(m->noise){m->buffer[m->tail++]=0x77;m->buffer[m->tail++]=0xaa;m->buffer[m->tail++]=0x00;}
    memcpy(m->buffer+m->tail,f,11);m->tail+=11;
}
static bool prepare(void *p){struct mock*m=p;++m->prepared;return !m->prepare_fail;}
static bool reset(void *p,bool asserted){struct mock*m=p;(void)asserted;++m->reset_calls;return !m->reset_fail;}
static bool healthy(void *p){struct mock*m=p;return !m->health_at||m->writes<m->health_at;}
static bool cancelled(void *p){struct mock*m=p;return m->cancel_at&&m->writes>=m->cancel_at;}
static bool now(void *p,uint64_t*t){struct mock*m=p;*t=m->reverse&&m->writes>4?0:m->time;return true;}
static bool pause_mock(void *p,unsigned t){struct mock*m=p;if(!m->frozen)m->time+=t;return true;}
static bool flush(void *p){struct mock*m=p;m->head=m->tail=0;return !m->flush_fail;}
static bool write_mock(void *p,const uint8_t*b,size_t n,unsigned timeout)
{
    struct mock*m=p;++m->writes;C(timeout>0);C(n==7||n==11);C(b[0]==0x55&&b[1]==0xaa);
    if(m->write_fail&&m->writes==m->write_fail)return false;
    unsigned op=b[2],a=b[4],reg=b[5];
    if(op==0x52){C(reg==0);for(unsigned i=0;i<m->chips;++i)response(m,0,0,0);}
    else if(op==0x40){C(a==m->assign*2);++m->assign;}
    else if(op==0x41||op==0x51){C(n==11);uint32_t v=(uint32_t)b[6]<<24|(uint32_t)b[7]<<16|(uint32_t)b[8]<<8|b[9];
        if(op==0x51){for(unsigned i=0;i<88;++i)m->registers[i][reg]=v;}
        else{C(a<176&&!(a&1));m->registers[a/2][reg]=v;}}
    else if(op==0x42){C(a<176&&!(a&1));uint32_t v=m->registers[a/2][reg];
        if(reg==8)v|=0x80000000;
        if(m->bad_pll&&reg==8&&a==174)v^=0x10;
        if(m->bad_ticket&&reg==0x14&&a==174)v^=1;
        response(m,a,reg,v);}
    else C(op==0x53);
    if(!m->frozen)++m->time;
    return true;
}
static int read_mock(void *p,uint8_t*b,size_t cap,unsigned timeout)
{
    struct mock*m=p;++m->reads;
    if(m->read_fail)return -1;if(m->oversize)return (int)cap+1;
    if(m->silent||m->head==m->tail){if(!m->frozen)m->time+=timeout;return 0;}
    size_t n=m->tail-m->head;if(n>cap)n=cap;if(n>m->chunk)n=m->chunk;
    memcpy(b,m->buffer+m->head,n);m->head+=n;return (int)n;
}
static bool off(void *p){struct mock*m=p;++m->off_calls;return !m->off_fail;}
static const struct hs88_start_ops ops={prepare,reset,healthy,cancelled,now,pause_mock,flush,write_mock,read_mock,off,NULL};
static struct mock fresh(void)
{
    struct mock m={.time=1000,.chips=88,.id_offset=2,.chunk=7};
    for(unsigned i=0;i<88;++i)m.registers[i][0x18]=0xc100;
    return m;
}
static struct hs88_start_result run(struct mock*m,unsigned timeout)
{
    const struct hs88_start_config c={150,timeout,100,m->id_offset};
    return hs88_start_chain(&c,&ops,m);
}
static void failed(struct mock*m,enum hs88_start_status want)
{
    struct hs88_start_result r=run(m,120000);
    if(r.status!=want)fprintf(stderr,"expected %s got %s stage %s\n",hs88_start_status_name(want),hs88_start_status_name(r.status),r.stage);
    C(r.status==want);C(m->off_calls==1);C(r.power_off_on_error==!m->off_fail);C(r.reset_asserted_on_error==!m->reset_fail);
}
int main(void)
{
    uint8_t b[9];struct hs_bm1362_command_result c=hs_bm1362_encode_chip_id_request(b,sizeof(b));
    const uint8_t golden[]={0x52,5,0,0,0x0a};C(c.status==HS_BM1362_COMMAND_OK&&c.written==5);C(!memcmp(b,golden,5));
    c=hs_bm1362_encode_probe(b,sizeof(b));C(c.status==HS_BM1362_COMMAND_OK&&b[3]==4);
    c=hs_bm1362_encode_chip_id_request(b,4);C(c.status!=HS_BM1362_COMMAND_OK);
    for(unsigned offset=2;offset<=3;++offset)for(unsigned chunk=1;chunk<=64;chunk*=8){
        struct mock m=fresh();m.id_offset=offset;m.chunk=chunk;m.noise=true;
        struct hs88_start_result r=run(&m,120000);
        if(r.status)fprintf(stderr,"start %s %s\n",hs88_start_status_name(r.status),r.stage);
        C(r.status==HS88_START_OK);C(r.discovered==88&&r.assigned==88&&r.configured==88);
        C(m.off_calls==0&&m.reset_calls==2);C(r.tx_bytes>0&&r.rx_bytes>0);
        for(unsigned i=0;i<88;++i){C(m.registers[i][8]==r.pll);C(m.registers[i][0x3c]==0x800082aa);}
    }
#define FAIL(field,value,status) do{struct mock m=fresh();m.field=value;failed(&m,status);}while(0)
    FAIL(chips,87,HS88_START_COUNT);FAIL(chips,89,HS88_START_COUNT);FAIL(chips,0,HS88_START_COUNT);
    FAIL(bad_id,true,HS88_START_ID);FAIL(bad_address,true,HS88_START_READBACK);
    FAIL(bad_pll,true,HS88_START_READBACK);FAIL(bad_ticket,true,HS88_START_READBACK);
    FAIL(corrupt,true,HS88_START_COUNT);FAIL(silent,true,HS88_START_COUNT);
    FAIL(prepare_fail,true,HS88_START_INTERLOCK);FAIL(reset_fail,true,HS88_START_IO);
    FAIL(flush_fail,true,HS88_START_IO);FAIL(read_fail,true,HS88_START_IO);FAIL(oversize,true,HS88_START_IO);
    FAIL(reverse,true,HS88_START_CLOCK);
    for(unsigned at=1;at<1000;at+=37){FAIL(write_fail,at,HS88_START_IO);FAIL(cancel_at,at,HS88_START_CANCELLED);FAIL(health_at,at,HS88_START_INTERLOCK);}
    {struct mock m=fresh();m.frozen=true;m.silent=true;failed(&m,HS88_START_PROTOCOL);C(m.reads<=4096);}
    {struct mock m=fresh();m.bad_pll=true;m.off_fail=true;failed(&m,HS88_START_READBACK);}
    {struct mock m=fresh();struct hs88_start_result r=run(&m,1000);C(r.status==HS88_START_TIMEOUT&&m.off_calls==1);}
    {struct mock m=fresh();struct hs88_start_config c0={500,120000,100,2};struct hs88_start_result r=hs88_start_chain(&c0,&ops,&m);C(r.status==HS88_START_ARGUMENT&&m.prepared==0&&m.off_calls==0);}
    printf("PASS AML88 startup: %u assertions; synthetic 88-chip replies, no hardware\n",checks);
    return 0;
}
