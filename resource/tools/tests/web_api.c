#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "api.h"
#include "http.h"
#include "update_api.h"
#include "app_status.h"
#include "data_cmd.h"
api_response_t web_device_request(const char *method,const char *uri,const uint8_t *body,
    size_t size,uint8_t *output,size_t capacity){
    return (api_response_t){404,"application/octet-stream",0};
}
static uint32_t ticks;
static int mode=2, closes, operations;
static int slot_resets, slot_operations;
static bool tearoff_active, tearoff_cancelled, serial_tearoff;
static bool rf_only_boot;
static bool update_entering;
static unsigned update_requests;
bool web_update_entering(void) { return update_entering; }
api_response_t web_update_application_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *out, size_t capacity) {
    update_requests++;
    bool found = (!strcmp(method, "GET") && !strcmp(uri, "/api/update")) ||
        (!strcmp(method, "POST") && !strcmp(uri, "/api/update/enter"));
    return (api_response_t){found ? 200 : 404, "application/octet-stream", 0};
}
static bool serial_sniff;
static bool release_ok = true;
bool hf_radio_release(void) { return release_ok; }
bool hf_sniff_active(void) { return serial_sniff; }
bool ble_slave_rf_only(void) { return rf_only_boot; }
bool usb_cdc_tearoff_running(void) { return serial_tearoff; }
static int tearoff_starts;
bool web_tearoff_start(const uint8_t *body, size_t size) {
    if (size != 13 || !body || tearoff_active) return false;
    tearoff_active = true;
    tearoff_cancelled = false;
    tearoff_starts++;
    return true;
}

bool web_tearoff_active(void) { return tearoff_active; }
void web_tearoff_cancel(void) { tearoff_cancelled = true; }
size_t web_tearoff_state(uint8_t output[20]) {
    memset(output, 0, 20);
    output[0] = tearoff_active ? 2 : 3;
    return 20;
}

void web_slots_reset(void){slot_resets++;}

api_response_t web_slots_request(const char *operation,const uint8_t *body,size_t size,uint8_t *output,size_t capacity){
    slot_operations++;
    if(!strcmp(operation,"activate"))mode=2;
    return (api_response_t){200,"application/octet-stream",0};
}

uint16_t web_lf_command(uint16_t cmd,uint16_t size,const uint8_t *data,uint8_t *out,uint16_t *length) {
    *length=1;out[0]=99;return STATUS_LF_TAG_OK;
}

bool hf_reader_supports(uint16_t command) { return (command >= 2300 && command <= 2309 && command != 2303) || command == DATA_CMD_ST25TB_WRITE_BLOCK; }
uint16_t hf_reader_command(uint16_t command,uint16_t length,const uint8_t *data,uint8_t *out,size_t capacity,uint16_t *size) {
    assert(capacity >= 1); *size=1; out[0]=77; return STATUS_HF_TAG_OK;
}
volatile bool m_is_field_on;
bool g_is_tag_emulating;
uint32_t app_timer_cnt_get(void){return ticks;}
uint32_t app_timer_cnt_diff_compute(uint32_t now,uint32_t then){return (now-then)&0xffffff;}
int get_device_mode(void){return mode;}
void reader_mode_enter(void){mode=1;}
void tag_mode_enter(void){mode=2;}
void mf1_darkside_reset(void){}
void nfca_session_close(void){closes++;}
void nfca_session_open(void){}
bool nfca_session_active(void){return false;}
uint16_t nfca_prepare(uint16_t cmd,uint16_t size,const uint8_t *data){return STATUS_SUCCESS;}
void nfca_finish(uint16_t cmd){}
uint16_t nfca_command(uint16_t cmd,uint16_t size,uint8_t *data,uint8_t *out,uint16_t *length){
    operations++;*length=1;out[0]=42;return STATUS_HF_TAG_OK;
}
static uint8_t output[512];
static api_response_t api(const char *uri){return api_dispatch_request("POST",uri,NULL,0,output,sizeof(output));}
static void api_lifecycle(void){
    serial_sniff = true;
    assert(api("/api/session").status == 409);
    assert(api("/api/slots/session").status == 409);
    assert(api("/api/device/settings").status == 409);
    serial_sniff = false;
    serial_tearoff=true;
    assert(api("/api/session").status==409);
    assert(api("/api/device/settings").status==409);
    assert(api("/api/st25tb/tearoff/start?id=1").status==409);
    serial_tearoff=false;
    m_is_field_on=true;assert(api("/api/session").status==409);m_is_field_on=false;
    g_is_tag_emulating=true;assert(api("/api/session").status==409);g_is_tag_emulating=false;
    assert(api("/api/session").status==200 && mode==1 && web_session_active());
    assert(!memcmp(output,"{\"session\":1}",13));
    assert(api("/api/session").status==409);
    assert(api("/api/end?id=2").status==409 && mode==1);
    assert(api("/api/command/2027?id=1").status==200 && output[2]==42);
    operations=0;
    assert(api("/api/command/2000?id=1").status==200 && output[0]==0 && output[2]==42 && operations==1);
    assert(api("/api/command/2300?id=1").status==200 && output[2]==77 && operations==1);
    assert(api("/api/command/2311?id=1").status==200 && output[2]==77 && operations==1);
    assert(api("/api/command/2303?id=1").status==404 && operations==1);
    assert(api("/api/command/2009?id=1").status==404 && operations==1);
    assert(api("/api/command/3000?id=1").status==200 && output[1]==STATUS_LF_TAG_OK && output[2]==99 && operations==1);
    release_ok = false;
    assert(api("/api/command/3000?id=1").status == 200 && output[1] == STATUS_HF_ERR_STAT);
    release_ok = true;
    assert(api("/api/command/3001?id=1").status==404 && operations==1);
    assert(api("/api/command/2000?id=2").status==409 && operations==1);
    assert(api("/api/command/2000?id=1x").status==404);
    assert(api("/api/command/4294969296?id=1").status==404);
    assert(api("/api/command/2000?id=4294967297").status==404);
    assert(api("/api/end?id=-1").status==404);
    assert(api("/api/keepalive?id=+1").status==404);
    ticks+=16384*29;assert(api("/api/keepalive?id=1").status==200);
    ticks+=16384*29;web_session_poll();assert(web_session_active());
    ticks+=16384*2;web_session_poll();assert(!web_session_active() && mode==2);
    assert(api("/api/command/2000?id=1").status==409);
    assert(api("/api/session").status==200);
    assert(api("/api/end?id=2").status==200 && !web_session_active());
    mode=1;assert(api("/api/session").status==200);web_session_end();assert(mode==1);
    mode=2;int before=slot_resets;
    assert(api("/api/slots/session").status==200 && mode==2 && slot_resets==before+1);
    assert(api("/api/command/2000?id=4").status==409);
    assert(api("/api/slots/open?id=3").status==409 && slot_operations==0);
    assert(api("/api/slots/open?id=4").status==200 && mode==2 && slot_operations==1);
    assert(api("/api/slots/open?id=4x").status==404);
    assert(api("/api/end?id=4").status==200 && mode==2 && slot_resets==before+2);
    mode=1;assert(api("/api/slots/session").status==200 && mode==1);
    assert(api("/api/slots/activate?id=5").status==200 && mode==2);
    assert(api("/api/end?id=5").status==200 && mode==2);
    assert(api("/api/slots/session").status==200);
    before=slot_resets;ticks+=16384*31;web_session_poll();assert(!web_session_active() && slot_resets==before+1);
    assert(api("/api/session").status==200 && mode==1);
    const uint8_t request[13]={0xd0,2,0x33};
    assert(api_dispatch_request("POST","/api/st25tb/tearoff/start?id=8",request,sizeof(request),output,sizeof(output)).status==409);
    assert(api_dispatch_request("POST","/api/st25tb/tearoff/start?id=7",request,sizeof(request),output,sizeof(output)).status==200 && tearoff_starts==1);
    assert(api("/api/command/2311?id=7").status==409);
    assert(api("/api/st25tb/tearoff/state?id=7").status==200 && output[0]==2);
    assert(api("/api/st25tb/tearoff/cancel?id=7").status==200 && tearoff_cancelled);
    assert(api("/api/end?id=7").status==200 && web_session_active() && mode==1);
    tearoff_active=false;
    web_session_poll();
    assert(!web_session_active() && mode==2);
}
static http_response_t http(const char *text){
    http_request_t request;
    assert(http_parse_request((const uint8_t *)text,strlen(text),&request)!=0);
    return http_dispatch_request(&request,output);
}

int main(void){
    update_entering = true;
    assert(web_session_active());
    assert(api("/api/session").status == 409);
    assert(api_dispatch_request("GET", "/api/status", NULL, 0, output, sizeof(output)).status == 200);
    update_entering = false;
    assert(!web_session_active());
    rf_only_boot=true;
    assert(api_dispatch_request("GET","/api/status",NULL,0,output,sizeof(output)).status==200);
    const char *restricted[]={"/api/session","/api/slots/session","/api/settings",
        "/api/slots/activate?id=1","/api/command/2000?id=1"};
    for(unsigned i=0;i<sizeof(restricted)/sizeof(restricted[0]);i++)
        assert(api(restricted[i]).status==409);
    assert(mode==2 && !web_session_active() && !slot_operations && !operations && !slot_resets);
    rf_only_boot=false;
    api_lifecycle();

    http_response_t response = http("GET / HTTP/1.1\r\nHost: 192.168.16.1\r\n\r\n");
    assert(response.status == 200);
    assert(http("GET /update HTTP/1.1\r\nHost: 192.168.16.1\r\n\r\n").status == 200);
    assert(http("GET /update.html HTTP/1.1\r\nHost: 192.168.16.1\r\n\r\n").status == 200);
    assert(http("GET /api/update HTTP/1.1\r\nHost: 192.168.16.1\r\n\r\n").status == 200);
    assert(http("POST /api/update/enter HTTP/1.1\r\nHost: 192.168.16.1\r\nOrigin: http://192.168.16.1\r\n\r\n").status == 200);
    assert(update_requests == 2);
    assert(http("POST /api/update/enter HTTP/1.1\r\nHost: 192.168.16.1\r\nOrigin: https://evil.example\r\n\r\n").status == 403);
    assert(update_requests == 2);
    assert(strstr(response.header, "connect-src 'self' " EXPECTED_RESOURCE_ORIGIN ";") != NULL);
    assert(response.header_size < sizeof(response.header));
    assert(response.header_size == strlen(response.header));
    assert(strstr(response.header, "\r\n\r\n") != NULL);
    assert(http("GET / HTTP/1.1\r\nHost: 192.168.16.1:80\r\n\r\n").status == 200);
    assert(http("GET / HTTP/1.1\r\nHost: 192.168.7.1\r\n\r\n").status == 403);
    assert(http("POST /api/session HTTP/1.1\r\nHost: 192.168.16.1\r\nOrigin: http://192.168.16.1\r\n\r\n").status == 200);
    web_session_end();
    assert(http("POST /api/session HTTP/1.1\r\nHost: 192.168.16.1\r\nOrigin: http://192.168.7.1\r\n\r\n").status == 403);
    assert(http("POST /api/session HTTP/1.1\r\nHost: 192.168.16.1\r\nOrigin: https://evil.example\r\n\r\n").status==403);
    assert(http("GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n").status==403);
    assert(http("POST /api/session HTTP/1.1\r\nHost: 192.168.16.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n").status==400);
    assert(http("POST /api/session HTTP/1.1\r\nHost: 192.168.16.1\r\nTransfer-Encoding: chunked\r\n\r\n").status==400);
    http_request_t request;
    const char *partial="POST /api/session HTTP/1.1\r\nHost: 192.168.16.1\r\nContent-Length: 1\r\n\r\n";
    assert(http_parse_request((const uint8_t *)partial,strlen(partial),&request)==0);
    return 0;
}

uint16_t desfire_reader_command(uint16_t n,const uint8_t *p,uint8_t *out,uint16_t *len) { (void)n;(void)p;(void)out;*len=0;return 0x69; }
