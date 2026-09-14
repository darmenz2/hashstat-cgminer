/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "hashstat-aml88-linux.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static bool err(char *out,size_t n,const char *s)
{if(out&&n)snprintf(out,n,"%s",s);return false;}
static bool num(json_t *o,const char *k,double lo,double hi,double *out)
{
    json_t *v=json_object_get(o,k);
    if(!json_is_number(v))return false;
    double x=json_number_value(v);
    if(!isfinite(x)||x<lo||x>hi)return false;
    *out=x;return true;
}
static bool integer(json_t *o,const char *k,unsigned lo,unsigned hi,unsigned *out)
{
    json_t *v=json_object_get(o,k);json_int_t x;
    if(!json_is_integer(v)||(x=json_integer_value(v))<(json_int_t)lo||x>(json_int_t)hi)return false;
    *out=(unsigned)x;return true;
}
static bool str(json_t *o,const char *k,char *out,size_t cap,bool absolute)
{
    json_t *v=json_object_get(o,k);const char *s=json_string_value(v);
    if(!s||!*s||strlen(s)!=json_string_length(v)||strlen(s)>=cap||(absolute&&s[0]!='/'))return false;
    for(const unsigned char *p=(const unsigned char*)s;*p;++p)if(*p<32||*p==127)return false;
    if(strstr(s,"/../")||strstr(s,"/./"))return false;
    memcpy(out,s,strlen(s)+1);return true;
}
static int hex(char c)
{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
bool hs88_config_parse(json_t *o,struct hs88_linux_config *c,char *e,size_t n)
{
    static const char *const keys[]={"enabled","bench_ack","chain","model","gpiochip","psu_line",
        "reset_line","presence_line","tach_lines","pwm","pwm_period_ns","pwm_inversed",
        "sensor_bus","sensor_addresses","eeprom_address","eeprom_sha256","psu_bus","psu_dac",
        "voltage_min","voltage_max","max_temp_c","min_fan_rpm","frequency_mhz","chip_id_offset",
        "work_interval_ms","trace_path"};
    struct hs88_linux_config v={0};const char *k;json_t *item;
    if(!o||!c||!json_is_object(o))return err(e,n,"configuration must be an object");
    json_object_foreach(o,k,item){bool found=false;for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i)if(!strcmp(k,keys[i]))found=true;
        if(!found)return err(e,n,"unknown configuration key");}
    if(!json_is_true(json_object_get(o,"enabled")))return err(e,n,"AML88 is disabled (enabled must be true)");
    const char *ack=json_string_value(json_object_get(o,"bench_ack"));
    if(!ack||strlen(ack)!=json_string_length(json_object_get(o,"bench_ack"))||strcmp(ack,"I verified this NoPIC board, pin mapping, cooling and PSU setpoint"))return err(e,n,"explicit bench acknowledgement required");
#define INT(key,lo,hi,dst) if(!integer(o,key,lo,hi,&(dst)))return err(e,n,"invalid " key)
#define STR(key,dst,abs) if(!str(o,key,dst,sizeof(dst),abs))return err(e,n,"invalid " key)
    INT("chain",0,2,v.chain);STR("model",v.model,false);
    if(strcmp(v.model,"BHB42821")&&strcmp(v.model,"BHB42831"))return err(e,n,"initial Linux backend requires an explicitly verified NoPIC BHB42821/31");
    STR("gpiochip",v.gpiochip,true);STR("sensor_bus",v.sensor_bus,true);STR("psu_bus",v.psu_bus,true);
    STR("trace_path",v.trace_path,true);
    if(strncmp(v.gpiochip,"/dev/gpiochip",13)||strncmp(v.sensor_bus,"/dev/i2c-",9)||
       strncmp(v.psu_bus,"/dev/i2c-",9)||!strcmp(v.sensor_bus,v.psu_bus))return err(e,n,"distinct native GPIO/I2C paths required");
    if(strncmp(v.trace_path,"/tmp/",5)&&strncmp(v.trace_path,"/var/log/",9))return err(e,n,"trace must be under /tmp or /var/log");
    INT("psu_line",0,255,v.psu_line);INT("reset_line",0,255,v.reset_line);INT("presence_line",0,255,v.presence_line);
    INT("eeprom_address",0x50,0x57,v.eeprom_addr);INT("psu_dac",0,255,v.psu_dac);
    INT("pwm_period_ns",1000,1000000,v.pwm_period_ns);INT("min_fan_rpm",500,20000,v.min_fan_rpm);
    INT("frequency_mhz",50,200,v.frequency_mhz);INT("chip_id_offset",2,3,v.chip_id_offset);
    INT("work_interval_ms",100,1000,v.work_interval_ms);
    if(!num(o,"voltage_min",1,30,&v.voltage_min)||!num(o,"voltage_max",1,30,&v.voltage_max)||
       v.voltage_max<=v.voltage_min||v.voltage_max-v.voltage_min>1.0||
       !num(o,"max_temp_c",30,85,&v.max_temp_c))return err(e,n,"invalid temperature/voltage limits (rail window <= 1 V)");
    item=json_object_get(o,"pwm_inversed");if(!json_is_boolean(item))return err(e,n,"pwm_inversed must be boolean");v.pwm_inversed=json_is_true(item);
    json_t *a=json_object_get(o,"tach_lines");if(!json_is_array(a)||json_array_size(a)!=4)return err(e,n,"four fan tach lines required");
    unsigned lines[7]={v.psu_line,v.reset_line,v.presence_line};
    for(unsigned i=0;i<4;++i){item=json_array_get(a,i);if(!json_is_integer(item)||json_integer_value(item)<0||json_integer_value(item)>255)return err(e,n,"invalid tach line");v.tach_lines[i]=(unsigned)json_integer_value(item);lines[i+3]=v.tach_lines[i];}
    for(unsigned i=0;i<7;++i)for(unsigned j=0;j<i;++j)if(lines[i]==lines[j])return err(e,n,"GPIO lines must be distinct");
    a=json_object_get(o,"sensor_addresses");if(!json_is_array(a)||json_array_size(a)!=2)return err(e,n,"two TMP75 sensor addresses required");
    for(unsigned i=0;i<2;++i){item=json_array_get(a,i);if(!json_is_integer(item)||json_integer_value(item)<0x48||json_integer_value(item)>0x4f)return err(e,n,"invalid TMP75 address");v.sensor_addr[i]=(unsigned)json_integer_value(item);}
    if(v.sensor_addr[0]==v.sensor_addr[1])return err(e,n,"temperature sensors must be distinct");
    a=json_object_get(o,"pwm");if(!json_is_array(a)||json_array_size(a)!=2)return err(e,n,"two PWM directories required");
    for(unsigned i=0;i<2;++i){const char *p=json_string_value(json_array_get(a,i));if(!p||strlen(p)!=json_string_length(json_array_get(a,i))||strncmp(p,"/sys/class/pwm/",15)||strlen(p)>=sizeof(v.pwm[i])||strstr(p,"..")||strchr(p,'\n'))return err(e,n,"invalid PWM path");strcpy(v.pwm[i],p);}
    if(!strcmp(v.pwm[0],v.pwm[1]))return err(e,n,"PWM paths must be distinct");
    const char *h=json_string_value(json_object_get(o,"eeprom_sha256"));if(!h||strlen(h)!=64||json_string_length(json_object_get(o,"eeprom_sha256"))!=64)return err(e,n,"raw EEPROM SHA-256 required");
    bool nonzero=false;for(unsigned i=0;i<32;++i){int a1=hex(h[2*i]),b1=hex(h[2*i+1]);if(a1<0||b1<0)return err(e,n,"invalid EEPROM SHA-256");v.eeprom_sha256[i]=(uint8_t)(a1*16+b1);nonzero|=v.eeprom_sha256[i]!=0;}
    if(!nonzero)return err(e,n,"placeholder EEPROM SHA-256 is not valid");
    *c=v;if(e&&n)*e=0;return true;
#undef INT
#undef STR
}
bool hs88_config_load(const char *path,struct hs88_linux_config *c,char *e,size_t n)
{
    if(!path||path[0]!='/')return err(e,n,"absolute configuration path required");
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(fd<0)return err(e,n,"cannot open configuration");
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&022)||st.st_size>16384){close(fd);return err(e,n,"configuration must be a small root-owned non-writable-by-others regular file");}
    char data[16385];size_t used=0;
    while(used<sizeof(data)){ssize_t got=read(fd,data+used,sizeof(data)-used);if(got<0&&errno==EINTR)continue;if(got<0){close(fd);return err(e,n,"configuration read failed");}if(!got)break;used+=(size_t)got;}
    close(fd);if(used>16384)return err(e,n,"configuration too large");
    json_error_t je;json_t *o=json_loadb(data,used,JSON_REJECT_DUPLICATES,&je);
    if(!o)return err(e,n,"invalid JSON or duplicate configuration key");
    bool ok=hs88_config_parse(o,c,e,n);json_decref(o);return ok;
}
