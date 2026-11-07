#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "classic_job.h"
#include "app_status.h"
#include "data_cmd.h"
static unsigned calls, resets, sent, captured;
static uint16_t fail;
static bool mismatch;
void nfca_session_close(void) { resets++; }
void nfca_session_open(void) {}
uint16_t nfca_prepare(uint16_t c, uint16_t n, const uint8_t *p) { return STATUS_SUCCESS; }
void nfca_finish(uint16_t c) {}
uint16_t nfca_command(uint16_t c, uint16_t n, uint8_t *p, uint8_t *out, uint16_t *size) {
    *size=0;
    if (c == DATA_CMD_HF14A_SCAN) {
        *size=5; memcpy(out,(uint8_t[]){4,1,2,3,4},5); out[1]^=mismatch; return 0;
    }
    calls++;
    if (fail) return fail;
    if (c == DATA_CMD_MF1_COLLECT_OBSERVATIONS) {
        assert(n==12 && p[11]<=16); captured+=p[11]; *size=p[11]*12;
    } else if (c == DATA_CMD_MF1_READ_SECTOR) {
        assert(n==13); *size=66;
    } else { assert(c==DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK); *size=7; }
    memset(out,0,*size); return 0;
}
bool ble_stream_response(uint8_t *p, uint16_t n) { assert(n>=23 && p[2]==4 && p[3]==67); sent++; return true; }

int main(void) {
    uint8_t p[88]={1,1,4,1,2,3,4,10}, out[CLASSIC_JOB_EVENT_MAX]; uint32_t id;
    for (unsigned i=0;i<10;i++) { p[8+8*i]=i; p[9+8*i]=0x60; }
    for (unsigned n=0;n<sizeof(p);n++) assert(!classic_job_start(p,n,false,&id));
    assert(classic_job_start(p,sizeof(p),false,&id)); uint32_t first=id;
    assert(!classic_job_start(p,sizeof(p),false,&id));
    for (unsigned i=0;i<10;i++) classic_job_poll();
    assert(calls==4); // Bounded queue applies backpressure without repeating RF work.
    assert(classic_job_read(id+1,out,sizeof(out))==0);
    unsigned completed=0;
    while (classic_job_active()) {
        size_t size=classic_job_read(id,out,sizeof(out));
        if (size) { assert(size==79 && out[6]==completed); completed++; assert(out[12]==completed); }
        classic_job_poll();
    }
    assert(completed==10 && calls==10);
    assert(classic_job_start(p,sizeof(p),false,&id) && id!=first);
    classic_job_poll(); classic_job_poll();
    assert(classic_job_cancel(id)); classic_job_poll();
    assert(classic_job_read(id,out,sizeof(out))==79);
    assert(classic_job_read(id,out,sizeof(out))==13 && out[7]==3 && out[12]==1);
    assert(!classic_job_active());
    mismatch=true;
    uint8_t checks[sizeof(p)]; memcpy(checks,p,sizeof(p)); checks[1]=0;
    assert(classic_job_start(checks,sizeof(checks),false,&id)); classic_job_poll(); classic_job_poll();
    assert(classic_job_read(id,out,sizeof(out))==13 && out[10]==1 && out[7]==1 && out[12]==0);
    mismatch=false;
    assert(classic_job_start(p,sizeof(p),true,&id));
    for (unsigned i=0;i<20;i++) classic_job_poll();
    assert(sent==10 && !classic_job_active());
    uint8_t capture[]={1,2,4,1,2,3,4,0x60,0,255,255,255,255,255,255,0x60,4,0,16,0,42};
    assert(classic_job_start(capture,sizeof(capture),false,&id));
    for (unsigned i=0;i<5;i++) classic_job_poll();
    assert(captured==42);
    for (unsigned i=0;i<3;i++) assert(classic_job_read(id,out,sizeof(out))==13+(i==2?10:16)*12);
    assert(!classic_job_active() && out[7]==1 && out[12]==42);
    assert(classic_job_start(capture,sizeof(capture),false,&id));
    classic_job_poll(); classic_job_poll();
    assert(classic_job_read(id,out,sizeof(out))==205);
    mismatch=true; classic_job_poll();
    assert(classic_job_read(id,out,sizeof(out))==13 && out[7]==1 && out[10]==1 && out[12]==16);
    mismatch=false;
    assert(classic_job_start(capture,sizeof(capture),false,&id)); classic_job_poll();
    fail=STATUS_HF_TAG_NO; classic_job_poll();
    assert(classic_job_read(id,out,sizeof(out))==13 && out[7]==1 && out[12]==0 && out[10]==1);
    classic_job_reset(); assert(!classic_job_active());
    return 0;
}
