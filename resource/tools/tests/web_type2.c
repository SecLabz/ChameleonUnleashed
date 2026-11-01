#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "rc522.h"
#include "type2_commands.h"
#include "app_status.h"

static picc_14a_tag_t selected={.uid_len=7,.uid={4,1,2,3,4,5,6},.sak=0};
static uint8_t status, scan_status, expected_opcode;
static uint16_t response_size;
static unsigned scans, transfers, halts;
void pcd_14a_reader_fast_halt_tag(void) {halts++;}
uint8_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) {scans++;*tag=selected;return scan_status;}
uint8_t pcd_14a_reader_raw_cmd(bool open,bool wait,bool append,bool select,bool keep,bool crc,
    uint16_t timeout,uint16_t bits,uint8_t *sent,uint8_t *out,uint16_t *size,uint16_t capacity) {
    assert(!open && wait && append && !select && keep && crc && timeout==25);
    assert(sent[0]==expected_opcode && bits==(sent[0]==0x60?8:sent[0]==0x3a?24:16) && capacity==34*8);
    // The real raw command appends two CRC bytes to the caller's TX buffer.
    sent[bits/8]=0;sent[bits/8+1]=0;
    transfers++;memset(out,42,response_size);*size=response_size;return status;
}
int main(void) {
    uint8_t input[]={0,0,7,4,1,2,3,4,5,6},out[32];uint16_t length;
    const uint8_t commands[]={0x60,0x30,0x3c,0x39,0x3e};
    const uint16_t sizes[]={8,16,32,3,1};
    for(unsigned op=0;op<5;op++) {
        input[0]=op;expected_opcode=commands[op];response_size=sizes[op];
        assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_HF_TAG_OK && length==response_size && out[0]==42);
        response_size--;
        assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_HF_ERR_STAT && length==0);
    }
    unsigned before=transfers;input[0]=5;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_PAR_ERR && transfers==before);
    input[0]=3;input[1]=3;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_PAR_ERR && transfers==before);
    input[0]=0;input[1]=1;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_PAR_ERR && transfers==before);
    input[1]=0;input[3]^=1;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_HF_TAG_NO && transfers==before);
    input[3]^=1;selected.sak=8;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_HF_TAG_NO && transfers==before);
    selected.sak=0;scan_status=STATUS_HF_TAG_NO;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_HF_TAG_NO && transfers==before);
    scan_status=0;expected_opcode=0x60;status=STATUS_HF_ERR_CRC;response_size=8;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_HF_ERR_CRC && length==0);
    status=0;expected_opcode=0x3a;
    uint8_t fast[]={5,248,7,4,1,2,3,4,5,6,255};
    for(unsigned count=1;count<=8;count++) {
        fast[10]=fast[1]+count-1;response_size=count*4;
        assert(type2_read_command(sizeof(fast),fast,out,&length)==STATUS_HF_TAG_OK);
        assert(length==response_size);
    }
    before=transfers;
    fast[1]=8;fast[10]=7;
    assert(type2_read_command(sizeof(fast),fast,out,&length)==STATUS_PAR_ERR && transfers==before);
    fast[10]=16;
    assert(type2_read_command(sizeof(fast),fast,out,&length)==STATUS_PAR_ERR && transfers==before);
    fast[10]=15;response_size=31;
    assert(type2_read_command(sizeof(fast),fast,out,&length)==STATUS_HF_ERR_STAT && length==0);
    response_size=32;status=STATUS_HF_ERR_CRC;
    assert(type2_read_command(sizeof(fast),fast,out,&length)==STATUS_HF_ERR_CRC && length==0);
    unsigned previous=scans;
    assert(type2_read_command(0,NULL,out,&length)==STATUS_PAR_ERR && scans==previous);
    input[2]=10;
    assert(type2_read_command(sizeof(input),input,out,&length)==STATUS_PAR_ERR && scans==previous);
}
