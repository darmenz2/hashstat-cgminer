/* SPDX-License-Identifier: GPL-3.0-only
 * Linux bench backend. GPIO line numbers are supplied by an operator-reviewed
 * configuration, not inferred from a miner marketing name. Voltage is never
 * changed: APW12 setpoint and measured output must match the supplied limits.
 */
#define _GNU_SOURCE 1
#include "hashstat-aml88-linux.h"
#include "hs_aml_uart_linux.h"
#include "hs_pow.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/syscall.h>

/* Linux GPIO chardev v1 and I2C UAPI. Kept local for the ARM cross-toolchain. */
struct line_req { uint32_t offsets[64],flags;uint8_t values[64];char label[32];uint32_t lines;int fd; };
struct line_data {uint8_t values[64];};
struct event_req {uint32_t offset,handleflags,eventflags;char label[32];int fd;};
struct edge {uint64_t ns;uint32_t id;};
struct bus_msg {uint16_t addr,flags,len;uint8_t *buf;};
struct bus_xfer {struct bus_msg *msgs;uint32_t count;};
#define LINE_REQUEST _IOWR(0xb4,0x03,struct line_req)
#define EVENT_REQUEST _IOWR(0xb4,0x04,struct event_req)
#define LINE_GET _IOWR(0xb4,0x08,struct line_data)
#define LINE_SET _IOWR(0xb4,0x09,struct line_data)
_Static_assert(sizeof(struct line_req)==364,"GPIO v1 request ABI");
_Static_assert(sizeof(struct event_req)==48,"GPIO v1 event ABI");
_Static_assert(sizeof(struct edge)==16,"GPIO edge ABI");

struct hs88_linux {
    struct hs88_linux_config c;
    struct hs_aml_uart_session uart;
    struct hs_aml_uart_linux uart_linux;
    int gpio,psu,reset,present,tach[4],sensors,psubus,lock,trace,watch;
    pid_t watcher;
    pthread_t monitor;
    bool monitor_started,prepared;
    atomic_bool fault,stopping,error_ready;
    atomic_uint main_tick;
    char error[192];
};
static uint64_t now_ms(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;
}
static void sleep_ms(unsigned ms)
{
    struct timespec t={ms/1000,(long)(ms%1000)*1000000};
    while(nanosleep(&t,&t)&&errno==EINTR){}
}
static bool failure(struct hs88_linux *b,const char *message)
{
    /* Single writer before initialization; afterwards publish fault first and
     * leave the immutable message alone (monitor and main can fail together). */
    bool was=atomic_exchange(&b->fault,true);
    if(!was){snprintf(b->error,sizeof(b->error),"%s",message);atomic_store_explicit(&b->error_ready,true,memory_order_release);}
    return false;
}
void hs88_linux_progress(struct hs88_linux *b)
{if(b)atomic_store(&b->main_tick,(unsigned)now_ms());}
static bool gpio_set(int fd,unsigned value)
{struct line_data d={{0}};d.values[0]=(uint8_t)value;return fd>=0&&ioctl(fd,LINE_SET,&d)==0;}
static bool gpio_get(int fd,unsigned *v)
{struct line_data d={{0}};if(fd<0||ioctl(fd,LINE_GET,&d))return false;*v=d.values[0];return true;}
static int gpio_request(int fd,unsigned line,bool output,unsigned initial)
{
    struct line_req r={0};r.offsets[0]=line;r.flags=output?2:1;r.values[0]=(uint8_t)initial;r.lines=1;
    strcpy(r.label,"hashstat-aml88");if(ioctl(fd,LINE_REQUEST,&r))return -1;
    (void)fcntl(r.fd,F_SETFD,FD_CLOEXEC);return r.fd;
}
static int tach_request(int fd,unsigned line)
{
    struct event_req r={0};r.offset=line;r.handleflags=1;r.eventflags=2;strcpy(r.label,"hashstat-fan");
    if(ioctl(fd,EVENT_REQUEST,&r))return -1;
    if(fcntl(r.fd,F_SETFL,O_NONBLOCK)||fcntl(r.fd,F_SETFD,FD_CLOEXEC)){close(r.fd);return -1;}return r.fd;
}
static bool write_number(const char *path,unsigned value)
{
    int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    char text[32];int len=snprintf(text,sizeof(text),"%u\n",value);ssize_t done=write(fd,text,(size_t)len);
    bool ok=done==len;int saved=errno;close(fd);errno=saved;return ok;
}
static bool pwm_full(struct hs88_linux *b,const char *dir)
{
    char path[200];struct stat st;
    if(stat(dir,&st)) {
        char parent[180];snprintf(parent,sizeof(parent),"%s",dir);char *slash=strrchr(parent,'/');
        if(!slash||strncmp(slash,"/pwm",4)||!slash[4])return false;
        char *end;unsigned long index=strtoul(slash+4,&end,10);if(*end||index>31)return false;*slash=0;
        snprintf(path,sizeof(path),"%s/export",parent);
        if(!write_number(path,(unsigned)index)&&errno!=EBUSY)return false;sleep_ms(100);
    }
#define PWM(field,val) do{snprintf(path,sizeof(path),"%s/" field,dir);if(!write_number(path,val))return false;}while(0)
    PWM("enable",0);
    snprintf(path,sizeof(path),"%s/polarity",dir);
    int polarity=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(polarity<0)return false;
    bool polarity_ok=write(polarity,"normal\n",7)==7;close(polarity);
    if(!polarity_ok)return false;
    PWM("duty_cycle",0);PWM("period",b->c.pwm_period_ns);
    PWM("duty_cycle",b->c.pwm_inversed?0:b->c.pwm_period_ns);PWM("enable",1);
#undef PWM
    return true;
}
static int open_chr(const char *path)
{
    int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(fd<0)return -1;if(fstat(fd,&st)||!S_ISCHR(st.st_mode)){close(fd);errno=ENODEV;return -1;}return fd;
}
static bool bus(int fd,struct bus_msg *msgs,unsigned count)
{struct bus_xfer x={msgs,count};return ioctl(fd,0x0707,&x)==(int)count;}
static bool sensor_read(int fd,unsigned addr,uint8_t reg,uint8_t *data,unsigned length)
{
    struct bus_msg m[2]={{(uint16_t)addr,0,1,&reg},{(uint16_t)addr,1,(uint16_t)length,data}};
    return bus(fd,m,2);
}
static bool psu_byte(struct hs88_linux *b,uint8_t *byte,bool write_it)
{
    uint8_t data[2]={0x11,*byte};struct bus_msg m={0x10,write_it?0:1,write_it?2:1,write_it?data:byte};
    return bus(b->psubus,&m,1);
}
/* Read-only APW12 commands. Packet sum includes length/command/payload.
 * No voltage, calibration or watchdog-disable command is exposed here. */
static bool psu_query(struct hs88_linux *b,uint8_t command,uint8_t *payload,size_t *size)
{
    uint8_t req[6]={0x55,0xaa,4,command,(uint8_t)(4+command),0},raw[64]={0};
    for(unsigned i=0;i<6;++i)if(!psu_byte(b,&req[i],true))return false;
    sleep_ms(500);
    uint64_t deadline=now_ms()+500;
    unsigned attempts=0;
    do{if(++attempts>128||now_ms()>=deadline||!psu_byte(b,&raw[0],false))return false;
       if(raw[0]==0xf5)return false;}while(raw[0]!=0x55);
    if(!psu_byte(b,&raw[1],false)||raw[1]!=0xaa||!psu_byte(b,&raw[2],false))return false;
    unsigned n=(unsigned)raw[2]+2;if(n<6||n>sizeof(raw))return false;
    for(unsigned i=3;i<n;++i)if(now_ms()>=deadline||!psu_byte(b,&raw[i],false))return false;
    unsigned sum=0;for(unsigned i=2;i<n-2;++i)sum+=raw[i];
    if(raw[3]!=command||raw[n-2]!=(sum&255)||raw[n-1]!=(sum>>8)||n-6>*size)return false;
    *size=n-6;memcpy(payload,raw+4,*size);return true;
}
static bool temperatures(struct hs88_linux *b)
{
    for(unsigned i=0;i<2;++i){uint8_t raw[2];if(!sensor_read(b->sensors,b->c.sensor_addr[i],0,raw,2))return false;
        int16_t v=(int16_t)((unsigned)raw[0]<<8|raw[1]);double c=(double)v/256.0;
        if(c< -10||c>b->c.max_temp_c)return false;}
    return true;
}
static bool voltage(struct hs88_linux *b,bool setpoint)
{
    uint8_t data[16];size_t n=sizeof(data);if(!psu_query(b,setpoint?3:4,data,&n))return false;
    if(setpoint)return n>=1 && data[0]==b->c.psu_dac;
    if(n<2)return false;
    double v=((unsigned)data[0]+((unsigned)data[1]<<8)+0.8615)/63.017;
    return v>=b->c.voltage_min&&v<=b->c.voltage_max;
}
static bool tach_drain(struct hs88_linux *b,unsigned count[4],unsigned timeout)
{
    struct pollfd f[4];for(unsigned i=0;i<4;++i)f[i]=(struct pollfd){b->tach[i],POLLIN,0};
    int rc=poll(f,4,(int)timeout);if(rc<0)return errno==EINTR;
    for(unsigned i=0;i<4;++i){if(f[i].revents&(POLLHUP|POLLERR|POLLNVAL))return false;
        for(unsigned lim=0;lim<4096;++lim){struct edge ev;ssize_t n=read(f[i].fd,&ev,sizeof(ev));
            if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n!=sizeof(ev))return false;
            if(ev.id==2)++count[i];}}
    return true;
}
static bool fans_ok(struct hs88_linux *b,unsigned count[4],uint64_t elapsed)
{
    if(!elapsed)return false;
    for(unsigned i=0;i<4;++i)if((uint64_t)count[i]*30000/elapsed<b->c.min_fan_rpm)return false;
    return true;
}
static bool off(struct hs88_linux *b)
{
    bool p=b->psu<0||gpio_set(b->psu,1);
    bool r=b->reset<0||gpio_set(b->reset,0);
    return p&&r;
}
static bool feed_watch(struct hs88_linux *b)
{char beat='K';return b->watch>=0&&send(b->watch,&beat,1,MSG_NOSIGNAL)==1;}
/* A separate process can still switch the PSU off if the mining thread or
 * I2C syscall stalls. It is not a replacement for a hardware PSU watchdog. */
static bool watchdog_start(struct hs88_linux *b)
{
    int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,pair))return false;
    int maxfd=1024;DIR *dir=opendir("/proc/self/fd");if(dir){struct dirent *de;while((de=readdir(dir))){int x=atoi(de->d_name);if(x>maxfd)maxfd=x;}closedir(dir);}
    pid_t pid=fork();if(pid<0){close(pair[0]);close(pair[1]);return false;}
    if(!pid){
        (void)syscall(SYS_setsid);
#ifdef SYS_prctl
        (void)syscall(SYS_prctl,15,"hs88-safeoff",0,0,0);
#endif
        struct sigaction sa={0};sa.sa_handler=SIG_IGN;sigemptyset(&sa.sa_mask);sigaction(SIGTERM,&sa,NULL);sigaction(SIGINT,&sa,NULL);
        for(int fd=0;fd<=maxfd;++fd)if(fd!=pair[1]&&fd!=b->psu&&fd!=b->reset&&fd!=b->lock)close(fd);
        uint64_t last=now_ms();
        for(;;){struct pollfd p={pair[1],POLLIN,0};int rc=poll(&p,1,100);char msg[64];
            if(rc<0&&errno!=EINTR)break;
            if(p.revents&(POLLHUP|POLLERR|POLLNVAL))break;
            if(p.revents&POLLIN){ssize_t n=read(pair[1],msg,sizeof(msg));if(n<=0)break;last=now_ms();}
            uint64_t t=now_ms();if(!t||t<last||t-last>=2500)break;
        }
        struct line_data d={{0}};d.values[0]=1;(void)syscall(SYS_ioctl,b->psu,LINE_SET,&d);
        d.values[0]=0;(void)syscall(SYS_ioctl,b->reset,LINE_SET,&d);
        /* Releasing a GPIO handle may revert its electrical state. Keep the
         * PSU-disable/reset handles and ownership lock until an operator has
         * disconnected hash power and explicitly terminates this guard. */
        close(pair[1]);
        for(;;)pause();
    }
    close(pair[1]);b->watch=pair[0];b->watcher=pid;
    int pf=open("/run/hashstat-aml88.guard.pid",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(pf>=0){char pidtext[32];int n=snprintf(pidtext,sizeof(pidtext),"%ld\n",(long)pid);(void)write(pf,pidtext,(size_t)n);close(pf);}
    return true;
}
static void *monitor_main(void *opaque)
{
    struct hs88_linux *b=opaque;unsigned edges[4]={0};uint64_t fan_start=now_ms(),last_v=fan_start,last_t=0;
    while(!atomic_load(&b->stopping)&&!atomic_load(&b->fault)){
        uint64_t now=now_ms();unsigned present;
        if(!now||(unsigned)((unsigned)now-atomic_load(&b->main_tick))>2000){failure(b,"mining loop stopped making progress");break;}
        if(!tach_drain(b,edges,20)){failure(b,"fan event input failed");break;}
        if(now-fan_start>=1500){if(!fans_ok(b,edges,now-fan_start)){failure(b,"fan speed below configured limit");break;}memset(edges,0,sizeof(edges));fan_start=now;}
        if(now-last_t>=500){if(!gpio_get(b->present,&present)||!present||!temperatures(b)){failure(b,"board presence or temperature interlock failed");break;}last_t=now;}
        if(now-last_v>=2000){if(!voltage(b,false)){failure(b,"APW12 voltage telemetry outside configured range");break;}last_v=now;}
        if(!feed_watch(b)){failure(b,"independent power-off guard disconnected");break;}
    }
    (void)off(b);return NULL;
}
/* TIOCEXCL does not revoke previously opened descriptors. Refuse another
 * userspace owner instead of killing processes or editing service scripts. */
static bool no_uart_owner(const char *path)
{
    DIR *proc=opendir("/proc");if(!proc)return false;struct dirent *de;bool ok=true;
    while(ok&&(de=readdir(proc))){char *end;long pid=strtol(de->d_name,&end,10);if(*end||pid<=0||pid==getpid())continue;
        char p[80];snprintf(p,sizeof(p),"/proc/%ld/fd",pid);DIR *fds=opendir(p);
        if(!fds){if(errno==EACCES)ok=false;continue;}struct dirent *fe;
        while((fe=readdir(fds))){if(fe->d_name[0]=='.')continue;char target[256],link[384];snprintf(link,sizeof(link),"%s/%s",p,fe->d_name);
            ssize_t n=readlink(link,target,sizeof(target)-1);if(n<0)continue;target[n]=0;
            if(!strcmp(target,path)){ok=false;break;}}
        closedir(fds);
    }
    closedir(proc);return ok;
}
static bool cancelled(void *p)
{struct hs88_linux *b=p;return atomic_load(&b->fault)||atomic_load(&b->stopping);}
static bool clock_callback(void *p,uint64_t *t)
{(void)p;*t=now_ms();return *t!=0;}
static bool healthy(void *p)
{struct hs88_linux *b=p;hs88_linux_progress(b);return !cancelled(p);}
static bool reset_callback(void *p,bool asserted)
{struct hs88_linux *b=p;if(!asserted&&cancelled(b))return false;hs88_linux_progress(b);return gpio_set(b->reset,asserted?0:1);}
static bool sleep_callback(void *p,unsigned ms)
{struct hs88_linux *b=p;hs88_linux_progress(b);sleep_ms(ms);return !cancelled(b);}
static bool flush_callback(void *p)
{struct hs88_linux *b=p;return b->uart.active&&ioctl(b->uart.fd,0x540b,0)==0;}
static bool write_callback(void *p,const uint8_t *data,size_t n,unsigned timeout)
{
    struct hs88_linux *b=p;hs88_linux_progress(b);
    struct hs_aml_uart_result r=hs_aml_uart_write_all(&b->uart,data,n,timeout);
    return r.status==HS_UART_OK&&r.transferred==n;
}
int hs88_linux_read(struct hs88_linux *b,uint8_t *data,size_t n,unsigned timeout)
{
    if(!b||!data||!n||n>65536||!b->uart.active||cancelled(b))return -1;
    hs88_linux_progress(b);struct pollfd f={b->uart.fd,POLLIN,0};int rc=poll(&f,1,(int)timeout);
    if(rc<0)return errno==EINTR?0:-1;if(!rc)return 0;
    if(f.revents&(POLLHUP|POLLERR|POLLNVAL))return -1;
    if(!(f.revents&POLLIN))return 0;
    struct hs_aml_uart_result r=hs_aml_uart_read_some(&b->uart,data,n,100);
    return r.status==HS_UART_OK?(int)r.transferred:-1;
}
static int read_callback(void *p,uint8_t *d,size_t n,unsigned t)
{return hs88_linux_read(p,d,n,t);}
static bool off_callback(void *p){return off(p);}
static void trace_callback(void *p,const char *stage,unsigned chip,const uint8_t *data,size_t n,bool rx)
{
    struct hs88_linux *b=p;if(b->trace<0)return;
    char line[2048];int used=snprintf(line,sizeof(line),"%llu %s chip=%u %s ",(unsigned long long)now_ms(),stage,chip,rx?"RX":"TX");
    for(size_t i=0;i<n && used<(int)sizeof(line)-4;++i)used+=snprintf(line+used,sizeof(line)-(size_t)used,"%02x",data[i]);
    line[used++]='\n';(void)write(b->trace,line,(size_t)used);
}
static bool prepare(void *p)
{
    struct hs88_linux *b=p;struct utsname u;uint8_t compat[1024]={0};int fd;struct stat st;
    if(b->prepared)return false;
    if(geteuid()!=0||uname(&u)||(strcmp(u.machine,"aarch64")&&strncmp(u.machine,"arm",3)))return failure(b,"Linux ARM root execution required");
    fd=open("/proc/device-tree/compatible",O_RDONLY|O_CLOEXEC);if(fd<0)return failure(b,"device tree unavailable");
    ssize_t got=read(fd,compat,sizeof(compat));close(fd);
    if(got<=0||!memmem(compat,(size_t)got,"amlogic",7))return failure(b,"Amlogic device-tree identity missing");
    const char *uartpath=hs_aml_uart_path(b->c.chain);
    if(!no_uart_owner(uartpath))return failure(b,"UART already owned; stop the other miner first");
    b->lock=open("/run/hashstat-aml88.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(b->lock<0||flock(b->lock,LOCK_EX|LOCK_NB))return failure(b,"AML88 ownership lock unavailable");
    b->gpio=open_chr(b->c.gpiochip);if(b->gpio<0)return failure(b,"GPIO chip open failed");
    b->psu=gpio_request(b->gpio,b->c.psu_line,true,1);if(b->psu<0)return failure(b,"PSU disable line unavailable");
    b->reset=gpio_request(b->gpio,b->c.reset_line,true,0);if(b->reset<0)return failure(b,"reset line unavailable");
    b->present=gpio_request(b->gpio,b->c.presence_line,false,0);unsigned present;
    if(b->present<0||!gpio_get(b->present,&present)||!present)return failure(b,"hashboard not present");
    for(unsigned i=0;i<2;++i)if(!pwm_full(b,b->c.pwm[i]))return failure(b,"cannot establish full fan PWM");
    for(unsigned i=0;i<4;++i){b->tach[i]=tach_request(b->gpio,b->c.tach_lines[i]);if(b->tach[i]<0)return failure(b,"fan tach line unavailable");}
    b->sensors=open_chr(b->c.sensor_bus);b->psubus=open_chr(b->c.psu_bus);
    if(b->sensors<0||b->psubus<0)return failure(b,"I2C adapter open failed");
    uint8_t eeprom[256],sum[32];
    if(!sensor_read(b->sensors,b->c.eeprom_addr,0,eeprom,256)||hs_sha256(eeprom,256,sum)!=HS_POW_OK||memcmp(sum,b->c.eeprom_sha256,32))return failure(b,"EEPROM fingerprint mismatch");
    if(!temperatures(b)||!voltage(b,true))return failure(b,"initial temperature or APW12 DAC does not match configuration");
    unsigned edges[4]={0};uint64_t start=now_ms();
    while(now_ms()-start<1500)if(!tach_drain(b,edges,20))return failure(b,"initial fan capture failed");
    if(!fans_ok(b,edges,now_ms()-start))return failure(b,"initial fan RPM too low");
    if(!watchdog_start(b))return failure(b,"cannot establish independent power-off guard");
    if(!feed_watch(b)||!gpio_set(b->psu,0))return failure(b,"power enable failed");
    sleep_ms(500);
    if(!voltage(b,false)||!temperatures(b)||!feed_watch(b))return failure(b,"post-enable voltage or temperature interlock failed");
    if(lstat(uartpath,&st)||!S_ISCHR(st.st_mode))return failure(b,"UART is not a character device");
    struct hs_aml_uart_linux_identity id={(uint64_t)st.st_dev,(uint64_t)st.st_ino,(uint32_t)major(st.st_rdev),(uint32_t)minor(st.st_rdev),true};
    if(!hs_aml_uart_linux_init(&b->uart_linux,b->c.chain,&id,cancelled,b))return failure(b,"UART adapter initialization failed");
    struct hs_aml_uart_readiness ready={true,true,true,true,true};
    struct hs_aml_uart_result ur=hs_aml_uart_open(&b->uart,hs_aml_uart_linux_ops(),&b->uart_linux,b->c.chain,ready,500);
    if(ur.status!=HS_UART_OK)return failure(b,"exclusive UART configuration failed");
    b->trace=open(b->c.trace_path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    if(b->trace<0||fstat(b->trace,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&022))return failure(b,"unsafe or unavailable trace file");
    hs88_linux_progress(b);
    if(pthread_create(&b->monitor,NULL,monitor_main,b))return failure(b,"safety monitor thread failed");
    b->monitor_started=true;b->prepared=true;return true;
}
static const struct hs88_start_ops ops={prepare,reset_callback,healthy,cancelled,clock_callback,sleep_callback,flush_callback,write_callback,read_callback,off_callback,trace_callback};
const struct hs88_start_ops *hs88_linux_start_ops(void){return &ops;}
struct hs88_linux *hs88_linux_new(const struct hs88_linux_config *c)
{
    if(!c)return NULL;struct hs88_linux *b=calloc(1,sizeof(*b));if(!b)return NULL;b->c=*c;
    b->gpio=b->psu=b->reset=b->present=b->sensors=b->psubus=b->lock=b->trace=b->watch=-1;
    for(unsigned i=0;i<4;++i)b->tach[i]=-1;
    b->uart=(struct hs_aml_uart_session)HS_AML_UART_SESSION_INIT;
    b->uart_linux=(struct hs_aml_uart_linux)HS_AML_UART_LINUX_INIT;
    atomic_init(&b->error_ready,false);atomic_init(&b->fault,false);atomic_init(&b->stopping,false);atomic_init(&b->main_tick,(unsigned)now_ms());return b;
}
bool hs88_linux_alive(struct hs88_linux *b){return b&&b->prepared&&!cancelled(b);}
struct hs_aml_uart_session *hs88_linux_uart(struct hs88_linux *b){return b?&b->uart:NULL;}
const char *hs88_linux_error(struct hs88_linux *b)
{return !b?"no context":atomic_load_explicit(&b->error_ready,memory_order_acquire)?b->error:"none";}
bool hs88_linux_shutdown(struct hs88_linux *b)
{
    if(!b)return true;atomic_store(&b->stopping,true);bool ok=off(b);
    if(b->monitor_started){pthread_join(b->monitor,NULL);b->monitor_started=false;}
    if(hs_aml_uart_close(&b->uart))ok=false;
    if(b->watch>=0){close(b->watch);b->watch=-1;}
    /* The guard intentionally outlives this runtime and retains SafeOff GPIO
     * ownership. No blocking wait and no automatic restart after a fault. */
    if(b->watcher>0){int status;pid_t rc=waitpid(b->watcher,&status,WNOHANG);if(rc!=0)ok=false;}
    b->prepared=false;return ok;
}
void hs88_linux_free(struct hs88_linux *b)
{
    if(!b)return;(void)hs88_linux_shutdown(b);
    int fds[]={b->gpio,b->psu,b->reset,b->present,b->sensors,b->psubus,b->lock,b->trace,b->tach[0],b->tach[1],b->tach[2],b->tach[3]};
    for(unsigned i=0;i<sizeof(fds)/sizeof(fds[0]);++i)if(fds[i]>=0)close(fds[i]);free(b);
}
#else
struct hs88_linux {int unused;};
struct hs88_linux *hs88_linux_new(const struct hs88_linux_config *c){(void)c;return NULL;}
void hs88_linux_free(struct hs88_linux *b){(void)b;}
const struct hs88_start_ops *hs88_linux_start_ops(void){return NULL;}
struct hs_aml_uart_session *hs88_linux_uart(struct hs88_linux *b){(void)b;return NULL;}
bool hs88_linux_alive(struct hs88_linux *b){(void)b;return false;}
void hs88_linux_progress(struct hs88_linux *b){(void)b;}
int hs88_linux_read(struct hs88_linux *b,uint8_t *d,size_t n,unsigned t){(void)b;(void)d;(void)n;(void)t;return -1;}
bool hs88_linux_shutdown(struct hs88_linux *b){(void)b;return true;}
const char *hs88_linux_error(struct hs88_linux *b){(void)b;return "Linux ARM backend required";}
#endif
