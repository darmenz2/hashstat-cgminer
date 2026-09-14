/* SPDX-License-Identifier: GPL-3.0-only */
#include "hashstat-aml88-linux.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define C(x) do {++checks;assert(x);} while(0)
/* Synthetic addresses/fingerprint: a parser fixture, NOT a hardware preset. */
static json_t *fixture(void)
{
    const char *text="{\"enabled\":true,\"bench_ack\":\"I verified this NoPIC board, pin mapping, cooling and PSU setpoint\","
        "\"chain\":0,\"model\":\"BHB42821\",\"gpiochip\":\"/dev/gpiochip1\","
        "\"psu_line\":1,\"reset_line\":2,\"presence_line\":3,\"tach_lines\":[4,5,6,7],"
        "\"pwm\":[\"/sys/class/pwm/pwmchip0/pwm0\",\"/sys/class/pwm/pwmchip0/pwm1\"],"
        "\"pwm_period_ns\":10000,\"pwm_inversed\":false,\"sensor_bus\":\"/dev/i2c-0\","
        "\"sensor_addresses\":[72,76],\"eeprom_address\":80,"
        "\"eeprom_sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\","
        "\"psu_bus\":\"/dev/i2c-1\",\"psu_dac\":100,\"voltage_min\":12.0,\"voltage_max\":12.5,"
        "\"max_temp_c\":70,\"min_fan_rpm\":1000,\"frequency_mhz\":150,\"chip_id_offset\":2,"
        "\"work_interval_ms\":200,\"trace_path\":\"/tmp/hashstat-aml88-test.log\"}";
    json_error_t e;json_t *o=json_loads(text,0,&e);C(o!=NULL);return o;
}
static void reject(json_t *o)
{
    struct hs88_linux_config c,old;memset(&c,0xa5,sizeof(c));old=c;char error[192]={0};
    C(!hs88_config_parse(o,&c,error,sizeof(error)));C(error[0]!=0);C(!memcmp(&c,&old,sizeof(c)));json_decref(o);
}
unsigned hs_test_aml88_config(void)
{
    checks=0;struct hs88_linux_config c;char error[192];json_t *o=fixture();
    C(hs88_config_parse(o,&c,error,sizeof(error)));C(c.chain==0&&c.frequency_mhz==150&&c.sensor_addr[1]==76);
    const char *key;json_t *v;
    json_object_foreach(o,key,v){json_t *bad=json_deep_copy(o);C(bad!=NULL);json_object_del(bad,key);reject(bad);}
    json_decref(o);
#define BAD(key,value) do{o=fixture();C(json_object_set_new(o,key,value)==0);reject(o);}while(0)
    BAD("enabled",json_false());BAD("enabled",json_integer(1));BAD("extra",json_true());
    BAD("model",json_string("s19-88"));BAD("model",json_string("BHB42801"));BAD("model",json_string("BHB42811"));
    BAD("chain",json_integer(3));BAD("frequency_mhz",json_integer(201));BAD("frequency_mhz",json_real(100));
    BAD("chip_id_offset",json_integer(0));BAD("voltage_max",json_real(14));BAD("max_temp_c",json_real(99));
    BAD("min_fan_rpm",json_integer(0));BAD("psu_dac",json_integer(256));BAD("bench_ack",json_string("yes"));
    BAD("reset_line",json_integer(1));BAD("sensor_bus",json_string("/dev/i2c-1"));BAD("gpiochip",json_string("/dev/mem"));
    BAD("trace_path",json_string("/etc/passwd"));BAD("eeprom_sha256",json_string("0000000000000000000000000000000000000000000000000000000000000000"));
    BAD("tach_lines",json_pack("[i,i,i,i]",4,5,6,6));BAD("sensor_addresses",json_pack("[i,i]",72,72));
    BAD("pwm",json_pack("[s,s]","/sys/class/pwm/../x","/sys/class/pwm/y"));
    o=fixture();C(json_object_set_new(o,"model",json_string("BHB42831"))==0);
    C(hs88_config_parse(o,&c,error,sizeof(error)));json_decref(o);
    C(!hs88_config_load("relative.json",&c,error,sizeof(error)));
    C(!hs88_config_parse(NULL,&c,error,sizeof(error)));
    printf("PASS AML88 configuration: %u assertions; no hardware opened\n",checks);
    return checks;
}
