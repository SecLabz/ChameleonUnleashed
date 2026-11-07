#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "classic_batch.h"
#include "data_cmd.h"
#include "app_status.h"
static unsigned prepares, commands, finishes, scans, opens, closes, entry;
static uint16_t status[8];
static uint8_t uid[4] = {1,2,3,4};
static bool mismatch;
bool nfca_session_reading(void) { return false; }
bool nfca_session_active(void) { return false; }
void nfca_session_close(void) { closes++; }
void nfca_session_open(void) { opens++; }
uint16_t nfca_prepare(uint16_t cmd, uint16_t n, const uint8_t *p) {
    prepares++; return STATUS_SUCCESS;
}
void nfca_finish(uint16_t cmd) { finishes++; }
uint16_t nfca_command(uint16_t cmd, uint16_t n, uint8_t *p, uint8_t *out, uint16_t *length) {
    commands++;
    if (cmd == DATA_CMD_HF14A_SCAN) {
        scans++; out[0] = 4; memcpy(out+1,uid,4); out[1] ^= mismatch; *length = 5;
        return STATUS_HF_TAG_OK;
    }
    if (cmd == DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK) {
        assert(n == 9 && p[1] == 0x60 && p[2] == 1 && p[3] == 0xff);
        *length = 7; memset(out, 0, 7); out[0] = status[entry] == 0;
    } else {
        assert(cmd == DATA_CMD_MF1_READ_SECTOR && n == 13 && p[8] == 4 && !memcmp(p+9,uid,4));
        *length = status[entry] ? 0 : 2 + (p[1] < 32 ? 4 : 16) * 16;
        memset(out, 0x5a, *length);
        if (*length) { out[0] = p[1] < 32 ? 0 : 255; out[1] = p[1] < 32 ? 15 : 255; }
    }
    return status[entry++];
}
static void reset(void) { prepares=commands=finishes=scans=opens=closes=entry=0; memset(status,0,sizeof(status)); }
int main(void) {
    uint8_t request[72] = {1,0,4,1,2,3,4,8}; uint8_t out[512]; uint16_t length;
    for (unsigned i=0;i<8;i++) {
        request[8+8*i]=i*4; request[9+8*i]=0x60; memset(request+10+8*i,255,6);
    }
    for (unsigned n=0;n<sizeof(request);n++) {
        reset(); assert(classic_batch_request(request,n,out,sizeof(out),&length)==STATUS_PAR_ERR && commands==0);
    }
    reset(); status[0]=STATUS_MF_ERR_AUTH; status[7]=STATUS_MF_ERR_AUTH;
    assert(classic_batch_request(request,sizeof(request),out,sizeof(out),&length)==0);
    assert(commands==9 && scans==1 && entry==8 && opens==1 && length==91);
    assert(out[0]==1 && out[1]==0 && out[2]==8 && out[4]==6);
    reset(); mismatch=true;
    assert(classic_batch_request(request,sizeof(request),out,sizeof(out),&length)==STATUS_HF_TAG_NO && entry==0);
    mismatch=false; reset(); status[2]=STATUS_HF_TAG_NO;
    assert(classic_batch_request(request,sizeof(request),out,sizeof(out),&length)==0 && entry==3 && out[2]==3);
    request[1]=1; request[7]=6;
    for (unsigned i=0;i<6;i++) request[8+8*i]=i;
    reset(); assert(classic_batch_request(request,56,out,sizeof(out),&length)==0 && length==423 && scans==0 && entry==6);
    reset(); assert(classic_batch_request(request,56,out,422,&length)==STATUS_PAR_ERR && commands==0);
    request[7]=1; request[8]=39;
    reset(); assert(classic_batch_request(request,16,out,sizeof(out),&length)==0 && length==265);
    request[8]=40;
    reset(); assert(classic_batch_request(request,16,out,sizeof(out),&length)==STATUS_PAR_ERR && commands==0);
    request[8]=0; request[9]=0x62;
    assert(classic_batch_request(request,16,out,sizeof(out),&length)==STATUS_PAR_ERR && commands==0);
    return 0;
}
