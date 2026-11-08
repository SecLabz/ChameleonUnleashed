#include "st25tb/model.h"
#define ST25TB_RECORD_SIZE 80u
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "slot_api.h"
#include "slot_image.h"
#include "app_status.h"
#include "tag_emulation.h"
#include "tag_persistence.h"
#include "fds_ids.h"
#include "nfc_mf1.h"
#include "nfc_mf0_ntag.h"
#include "rfid_main.h"

#define ASSERT(condition) assert(condition)
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_ERROR(...) ((void)0)
static tag_slot_config_t slotConfig;
static uint16_t m_slot_config_crc, hf_crc, lf_crc;
static uint32_t hf_words[1125], lf_words[5];
static tag_data_buffer_t hf_buffer = {sizeof(hf_words),(uint8_t *)hf_words,&hf_crc};
static tag_data_buffer_t lf_buffer = {sizeof(lf_words),(uint8_t *)lf_words,&lf_crc};
static struct { uint16_t size; uint8_t data[4500]; } records[33];
static unsigned writes, fail_at, loads;
static bool senses[3], shadow_hf;
static unsigned hf_sense_changes;
static bool slot_sense_held, slot_sense_requested[3];
static unsigned critical_depth;
#define CRITICAL_REGION_ENTER() do { critical_depth++; } while (0)
#define CRITICAL_REGION_EXIT() do { assert(critical_depth); critical_depth--; } while (0)
#define APP_ERROR_CHECK(error) assert((error)==0)
#define NRF_ERROR_INVALID_PARAM 1
#define TAG_FIELD_LED_OFF() ((void)0)
bool g_is_tag_emulating;
static int mode = DEVICE_MODE_TAG;
static unsigned index_of(uint16_t id, uint16_t key) {
    if (id == FDS_EMULATION_CONFIG_FILE_ID) return 0;
    unsigned index = id >= FDS_SLOT_TAG_NICK_NAME_FILE_ID_BASE ? 17 + (id-FDS_SLOT_TAG_NICK_NAME_FILE_ID_BASE)*2 : 1 + (id-FDS_SLOT_TAG_DUMP_FILE_ID_BASE)*2;
    assert(key == 1 || key == 2); index += key-1; assert(index < 33); return index;
}
/* PRODUCTION_HASH */
uint32_t fds_hash_sync(uint16_t id,uint16_t key,uint32_t hash) {
    unsigned index=index_of(id,key);
    return fds_hash_bytes(hash, records[index].data, records[index].size);
}
bool is_slot_enabled(uint8_t slot,tag_sense_type_t sense) {
    return sense==TAG_SENSE_HF?slotConfig.slots[slot].enabled_hf:slotConfig.slots[slot].enabled_lf;
}
bool fds_write_sync(uint16_t id,uint16_t key,uint16_t length,void *buffer) {
    writes++; if (fail_at && writes == fail_at) return false;
    unsigned index=index_of(id,key), size=(length+3)&~3u;
    assert(size <= sizeof(records[index].data)); records[index].size=size;
    memcpy(records[index].data,buffer,size); return true;
}
bool fds_read_sync(uint16_t id,uint16_t key,uint16_t *length,uint8_t *buffer) {
    unsigned index=index_of(id,key),size=records[index].size;
    if (!size || size > *length) {*length=0;return false;}
    memcpy(buffer,records[index].data,size);*length=size;return true;
}
bool fds_is_exists(uint16_t id,uint16_t key) {return records[index_of(id,key)].size != 0;}
int fds_delete_sync(uint16_t id,uint16_t key) {
    unsigned index=index_of(id,key);bool present=records[index].size != 0;records[index].size=0;return present;
}
void get_fds_map_by_slot_sense_type_for_dump(uint8_t slot,tag_sense_type_t sense,fds_slot_record_map_t *map) {
    *map=(fds_slot_record_map_t){sense,FDS_SLOT_TAG_DUMP_FILE_ID_BASE+slot};
}
void get_fds_map_by_slot_sense_type_for_nick(uint8_t slot,tag_sense_type_t sense,fds_slot_record_map_t *map) {
    *map=(fds_slot_record_map_t){sense,FDS_SLOT_TAG_NICK_NAME_FILE_ID_BASE+slot};
}
int get_device_mode(void) {return mode;}
void tag_mode_enter(void) {mode=DEVICE_MODE_TAG;}
static bool rf_write_on_resume;
static unsigned crypto_prepares;

void mf1_emulator_crypto_prepare(void) { crypto_prepares++; }

void nfc_tag_14a_sense_switch(bool enabled) {
    tag_specific_type_t type = slotConfig.slots[slotConfig.active_slot].tag_hf;
    if (enabled && type >= TAG_TYPE_MIFARE_Mini && type <= TAG_TYPE_MIFARE_4096) {
        assert(crypto_prepares);
        crypto_prepares--;
    }
    hf_sense_changes++;
    senses[TAG_SENSE_HF]=enabled;
    if (enabled && rf_write_on_resume) {((uint8_t *)hf_words)[300]++;rf_write_on_resume=false;}
}
void lf_tag_125khz_sense_switch(bool enabled) {senses[TAG_SENSE_LF]=enabled;}
uint8_t tag_emulation_get_slot(void) {return slotConfig.active_slot;}
void tag_emulation_set_slot(uint8_t slot) {slotConfig.active_slot=slot;}
void tag_emulation_get_specific_types_by_slot(uint8_t slot,tag_slot_specific_type_t *types) {
    *types=(tag_slot_specific_type_t){slotConfig.slots[slot].tag_hf,slotConfig.slots[slot].tag_lf};
}
void apply_slot_change(uint8_t previous,uint8_t slot) {}
void light_up_by_slot(void) {}
void calc_14a_crc_lut(uint8_t *bytes,size_t size,uint8_t *result) {
    uint16_t sum=0;for(size_t i=0;i<size;i++)sum+=bytes[i];memcpy(result,&sum,2);
}
/* PRODUCTION_GEOMETRY */
tag_sense_type_t get_sense_type_from_tag_type(tag_specific_type_t type) {
    slot_image_shape_t shape;return slot_image_shape(type,&shape)?shape.sense:TAG_SENSE_NO;
}
tag_data_buffer_t *get_buffer_by_tag_type(tag_specific_type_t type) {
    tag_sense_type_t sense=get_sense_type_from_tag_type(type);
    return sense==TAG_SENSE_HF?&hf_buffer:sense==TAG_SENSE_LF?&lf_buffer:NULL;
}
/* PRODUCTION_ST25_SAVE */
static int save_size(tag_specific_type_t type,tag_data_buffer_t *buffer) {
    if (type == TAG_TYPE_ST25TB512_AT) return nfc_st25tb_data_savecb(type,buffer);
    slot_image_shape_t shape;
    if (shadow_hf && get_sense_type_from_tag_type(type)==TAG_SENSE_HF) return 0;
    return slot_image_shape(type,&shape)?shape.native_bytes:0;
}
static tag_datas_savecb_t get_data_savecb_from_tag_type(tag_specific_type_t type) {return save_size;}
#define m_tag_data_buffer_hf hf_words
#define m_tag_data_buffer_lf lf_words
#define m_tag_data_hf_crc hf_crc
#define m_tag_data_lf_crc lf_crc
static int load_size(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    slot_image_shape_t shape;
    if (!slot_image_shape(type,&shape)) return 0;
    if (buffer->length >= shape.native_bytes) loads++;
    return shape.native_bytes;
}
static tag_datas_loadcb_t get_data_loadcb_from_tag_type(tag_specific_type_t type) {return load_size;}
/* PRODUCTION_STORAGE */
void tag_emulation_load_data(void) {
    load_data_by_tag_type(slotConfig.active_slot,slotConfig.slots[slotConfig.active_slot].tag_hf);
    load_data_by_tag_type(slotConfig.active_slot,slotConfig.slots[slotConfig.active_slot].tag_lf);
}
static uint8_t output[512], encoded[4500];
static uint32_t image_words[1125], decoded_words[1125];
static api_response_t raw_call(const char *op,const uint8_t *data,size_t size) {
    static const struct { const char *name; api_operation_t operation; } routes[] = {
        {"open", API_SLOT_OPEN}, {"read", API_SLOT_READ}, {"begin", API_SLOT_BEGIN},
        {"write", API_SLOT_WRITE}, {"commit", API_SLOT_COMMIT}, {"activate", API_SLOT_ACTIVATE},
        {"nick", API_SLOT_NICK}, {"enable", API_SLOT_ENABLE}, {"clear", API_SLOT_CLEAR}
    };
    api_operation_t operation = API_UNKNOWN_WRITE;
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        if (!strcmp(op, routes[i].name)) operation = routes[i].operation;
    return web_slots_request(operation,data,size,output,sizeof(output));
}
static void revision_bytes(uint8_t *bytes,uint8_t slot,tag_sense_type_t sense) {
    uint32_t revision=tag_emulation_place_revision(slot,sense);
    for(unsigned i=0;i<4;i++)bytes[i]=revision>>(24-8*i);
}
static api_response_t call(const char *op,const uint8_t *data,size_t size) {
    uint8_t guarded[64];
    if(!strcmp(op,"begin") && size==4) {
        memcpy(guarded,data,4);revision_bytes(guarded+4,data[0],data[1]);
        return raw_call(op,guarded,8);
    }
    if(!strcmp(op,"activate") && size==1) {
        guarded[0]=data[0];revision_bytes(guarded+1,data[0],TAG_SENSE_HF);revision_bytes(guarded+5,data[0],TAG_SENSE_LF);
        return raw_call(op,guarded,9);
    }
    if((!strcmp(op,"nick") || !strcmp(op,"enable") || !strcmp(op,"clear")) && size>=2) {
        assert(size+4<=sizeof(guarded));memcpy(guarded,data,2);revision_bytes(guarded+2,data[0],data[1]);
        memcpy(guarded+6,data+2,size-2);return raw_call(op,guarded,size+4);
    }
    return raw_call(op,data,size);
}
static void seed(uint8_t slot,tag_sense_type_t sense,tag_specific_type_t type,uint8_t value) {
    slot_image_shape_t shape;assert(slot_image_shape(type,&shape));
    memset(image_words,0,sizeof(image_words));
    if(sense==TAG_SENSE_HF) {
        nfc_tag_14a_coll_res_entity_t *id=(void *)image_words;id->size=4;memset(id->uid,value,4);id->sak[0]=8;id->atqa[0]=4;
        if(shape.unit_bytes==16) {
            nfc_tag_mf1_information_t *image=(void *)image_words;image->config.prng_type=1;
            memset(image->memory,value,shape.memory_bytes);
        } else {
            nfc_tag_mf0_ntag_information_t *image=(void *)image_words;memset(image->memory,value,shape.memory_bytes);
        }
    } else memset(image_words,value,shape.native_bytes);
    fds_slot_record_map_t map;get_fds_map_by_slot_sense_type_for_dump(slot,sense,&map);
    assert(fds_write_sync(map.id,map.key,shape.native_bytes,image_words));
    if(sense==TAG_SENSE_HF){slotConfig.slots[slot].tag_hf=type;slotConfig.slots[slot].enabled_hf=true;}
    else {slotConfig.slots[slot].tag_lf=type;slotConfig.slots[slot].enabled_lf=true;}
}
static uint16_t make_upload(void) {
    memset(image_words,0,sizeof(image_words));
    nfc_tag_mf1_information_t *image=(void *)image_words;
    image->res_coll.size=7;memset(image->res_coll.uid,0xab,7);image->res_coll.atqa[0]=4;image->res_coll.sak[0]=0x18;
    image->res_coll.ats.length=3;memcpy(image->res_coll.ats.data,"ATS",3);
    image->config.mode_gen1a_magic=true;image->config.mode_gen2_magic=true;
    image->config.mode_block_write=3;image->config.prng_type=2;
    for(unsigned i=0;i<4096;i++)((uint8_t *)image->memory)[i]=i;
    uint16_t size;assert(slot_image_encode(TAG_TYPE_MIFARE_4096,(uint8_t *)image_words,sizeof(image_words),encoded,sizeof(encoded),&size));
    slot_image_shape_t shape;assert(slot_image_decode(encoded,size,(uint8_t *)decoded_words,sizeof(decoded_words),&shape));
    assert(shape.native_bytes<=sizeof(image_words));assert(!memcmp(image_words,decoded_words,shape.native_bytes));
    return size;
}
static void upload(uint16_t size, uint8_t slot, tag_sense_type_t sense) {
    uint8_t begin[]={slot,sense,size>>8,size};assert(call("begin",begin,4).status==200);
    uint8_t wrong[]={0,1,0};assert(call("write",wrong,3).status==400);
    assert(call("commit",(uint8_t[]){1,1,'x'},3).status==409);
    for(unsigned offset=0;offset<size;offset+=500) {
        uint8_t data[502];unsigned count=size-offset<500?size-offset:500;
        data[0]=offset>>8;data[1]=offset;memcpy(data+2,encoded+offset,count);
        assert(call("write",data,count+2).status==200 && ((output[0]<<8)|output[1])==offset+count);
    }
}
static void roundtrips(void) {
    for(unsigned type=100;type<=1108;type++) {
        slot_image_shape_t shape;if(!slot_image_shape(type,&shape))continue;
        memset(image_words,0,sizeof(image_words));
        if(shape.sense==TAG_SENSE_HF) {
            nfc_tag_14a_coll_res_entity_t *id=(void *)image_words;id->size=10;memset(id->uid,42,10);
        }
        uint16_t size;assert(slot_image_encode(type,(uint8_t *)image_words,sizeof(image_words),encoded,sizeof(encoded),&size));
        assert(slot_image_decode(encoded,size,(uint8_t *)decoded_words,sizeof(decoded_words),&shape));
        assert(!memcmp(image_words,decoded_words,shape.native_bytes));
        assert(!slot_image_decode(encoded,size-1,(uint8_t *)decoded_words,sizeof(decoded_words),&shape));
    }
}
int main(void) {
    /* Variable-size DESFire images use the existing v2 slot envelope. */
    uint8_t desfire[] = {7,4,1,2,3,4,5,6,0,10,0,0,'D','F','U','1',0x44,3,0x20,2,2,0,0,0};
    slot_image_shape_t df_shape;uint16_t df_size;
    assert(slot_image_encode(TAG_TYPE_DESFIRE,desfire,sizeof(desfire),encoded,sizeof(encoded),&df_size));
    assert(df_size==29);
    assert(slot_image_decode(encoded,df_size,(uint8_t *)decoded_words,sizeof(decoded_words),&df_shape));
    assert(df_shape.native_bytes==sizeof(desfire) && !memcmp(desfire,decoded_words,sizeof(desfire)));
    assert(!slot_image_decode(encoded,df_size-1,(uint8_t *)decoded_words,sizeof(decoded_words),&df_shape));
    encoded[0]=1;
    assert(!slot_image_decode(encoded,df_size,(uint8_t *)decoded_words,sizeof(decoded_words),&df_shape));
    roundtrips();
    /* NFC-V v2 envelopes carry unchanged legacy payloads, not NFC-A structs. */
    for (unsigned product=0;product<2;product++) {
        unsigned blocks=product?32:16, count=product?28:8;
        tag_specific_type_t type=product?TAG_TYPE_SLIX:TAG_TYPE_SLIX_L;
        uint8_t raw[192]={0xe0,4,product?1:3,product?0x10:0};
        for(unsigned i=12;i<32+blocks*4;i++)raw[i]=i*7;
        for(unsigned i=0;i<blocks;i++)raw[32+blocks*4+i]=i%2;
        uint16_t size;slot_image_shape_t shape;
        assert(slot_image_encode(type,raw,32+blocks*5,encoded,sizeof(encoded),&size));
        assert(size==37+blocks*5);
        assert(slot_image_decode(encoded,size,(uint8_t *)decoded_words,sizeof(decoded_words),&shape));
        assert(shape.native_bytes==32+blocks*5 && shape.memory_bytes==count*4);
        assert(!memcmp(raw,decoded_words,shape.native_bytes));
        assert(!slot_image_decode(encoded,size-1,(uint8_t *)decoded_words,sizeof(decoded_words),&shape));
        encoded[0]=1;
        assert(!slot_image_decode(encoded,size,(uint8_t *)decoded_words,sizeof(decoded_words),&shape));
    }
    /* Version 2 carries ST25TB's existing explicit image without NFC-A fields. */
    memset(image_words,0,sizeof(image_words));
    uint8_t *st25=(uint8_t *)image_words;
    st25[7]=0xd0;st25[6]=2;st25[5]=0x33;st25[76]=0x5a;
    for(unsigned i=8;i<76;i++)st25[i]=i;
    uint16_t st25_size;
    slot_image_shape_t st25_shape;
    assert(slot_image_encode(TAG_TYPE_ST25TB512_AT,st25,80,encoded,sizeof(encoded),&st25_size));
    assert(st25_size==82 && encoded[0]==2 && encoded[81]==0x5a);
    assert(slot_image_decode(encoded,st25_size,(uint8_t *)decoded_words,sizeof(decoded_words),&st25_shape));
    assert(st25_shape.native_bytes==80 && !memcmp(image_words,decoded_words,80));
    encoded[10]^=1;
    assert(!slot_image_decode(encoded,st25_size,(uint8_t *)decoded_words,sizeof(decoded_words),&st25_shape));
    encoded[10]^=1;
    assert(!slot_image_decode(encoded,st25_size-1,(uint8_t *)decoded_words,sizeof(decoded_words),&st25_shape));
    /* The real model owns live ST25 bytes, separate from the shared buffer. */
    slotConfig.active_slot=0;slotConfig.slots[0].tag_hf=TAG_TYPE_ST25TB512_AT;
    memcpy(hf_buffer.buffer,st25,80);
    assert(st25tb_load(st25));
    assert(!slot_sense_requested[TAG_SENSE_HF]);
    uint32_t initial_revision=tag_emulation_place_revision(0,TAG_SENSE_HF);
    st25[8]^=0x5a;
    assert(st25tb_load(st25));
    uint32_t changed_revision=tag_emulation_place_revision(0,TAG_SENSE_HF);
    assert(initial_revision!=changed_revision);
    assert(!slot_sense_requested[TAG_SENSE_HF]);
    uint16_t snapshot_size=sizeof(decoded_words);tag_specific_type_t snapshot_type;
    assert(tag_emulation_snapshot(0,TAG_SENSE_HF,(uint8_t *)decoded_words,&snapshot_size,&snapshot_type));
    assert(snapshot_type==TAG_TYPE_ST25TB512_AT && snapshot_size==sizeof(hf_words));
    assert(((uint8_t *)decoded_words)[8]==st25[8]);
    assert(hf_buffer.buffer[8]==st25[8]);
    seed(0,TAG_SENSE_HF,TAG_TYPE_MIFARE_1024,1);seed(0,TAG_SENSE_LF,TAG_TYPE_EM410X,2);
    seed(1,TAG_SENSE_HF,TAG_TYPE_MIFARE_Mini,3);seed(1,TAG_SENSE_LF,TAG_TYPE_VIKING,4);
    tag_emulation_load_data();tag_emulation_sense_run();
    /* Re-selecting the same small image after a larger slot must not hash
     * unused bytes left by the other card in the shared HF buffer. */
    assert(tag_emulation_select_checked(1,(uint8_t *)decoded_words,sizeof(decoded_words)));
    uint32_t mini_revision=tag_emulation_place_revision(1,TAG_SENSE_HF);
    uint32_t viking_revision=tag_emulation_place_revision(1,TAG_SENSE_LF);
    seed(0,TAG_SENSE_HF,TAG_TYPE_MIFARE_4096,1);
    seed(0,TAG_SENSE_LF,TAG_TYPE_IOPROX,2);
    assert(tag_emulation_select_checked(0,(uint8_t *)decoded_words,sizeof(decoded_words)));
    assert(tag_emulation_select_checked(1,(uint8_t *)decoded_words,sizeof(decoded_words)));
    assert(tag_emulation_place_revision(1,TAG_SENSE_HF)==mini_revision);
    assert(tag_emulation_place_revision(1,TAG_SENSE_LF)==viking_revision);
    seed(0,TAG_SENSE_HF,TAG_TYPE_MIFARE_1024,1);
    seed(0,TAG_SENSE_LF,TAG_TYPE_EM410X,2);
    assert(tag_emulation_select_checked(0,(uint8_t *)decoded_words,sizeof(decoded_words)));
    unsigned unchanged_hf=hf_sense_changes;
    tag_emulation_place_revision(0,TAG_SENSE_HF);
    assert(hf_sense_changes==unchanged_hf);
    unsigned before=writes;uint8_t place[]={1,TAG_SENSE_HF};
    assert(call("open",place,2).status==200 && tag_emulation_get_slot()==0 && writes==before);
    assert(call("read",(uint8_t[]){0,0,0,9},4).status==200 && output[0]==1);
    assert(call("read",(uint8_t[]){0xff,0xff,0,1},4).status==400);
    place[0]=0;assert(call("open",place,2).status==200 && writes==before && senses[1]&&senses[2]);
    uint8_t lf_saved[4];memcpy(lf_saved,records[index_of(0x1101,1)].data,4);
    for(unsigned failure=1;failure<=4;failure++) {
        fail_at=0;seed(1,TAG_SENSE_HF,TAG_TYPE_MIFARE_Mini,3);
        uint16_t size=make_upload();upload(size,1,TAG_SENSE_HF);before=writes;fail_at=writes+failure;
        assert(call("commit",(uint8_t[]){1,1,'x'},3).status==200 && output[1]==STATUS_FLASH_WRITE_FAIL);
        assert(!memcmp(lf_saved,records[index_of(0x1101,1)].data,4));
        assert(slotConfig.slots[1].tag_lf==TAG_TYPE_VIKING && slotConfig.slots[1].enabled_lf);
        if(failure>1)assert(slotConfig.slots[1].tag_hf==TAG_TYPE_UNDEFINED && !slotConfig.slots[1].enabled_hf);
        assert(call("read",(uint8_t[]){0,0,0,1},4).status==409);
    }
    fail_at=0;uint16_t size=make_upload();upload(size,1,TAG_SENSE_HF);
    api_response_t committed=call("commit",(uint8_t[]){1,6,'E','n','t','r',0xc3,0xa9},8);
    if(committed.status!=200 || output[1]!=STATUS_SUCCESS)
        fprintf(stderr,"Commit status %u, operation status %u, writes %u, fail at %u, type %u\n",committed.status,output[1],writes,fail_at,slotConfig.slots[1].tag_hf);
    assert(committed.status==200 && output[1]==STATUS_SUCCESS);
    assert(slotConfig.slots[1].tag_hf==TAG_TYPE_MIFARE_4096 && slotConfig.slots[1].enabled_hf && tag_emulation_get_slot()==0);
    place[0]=1;assert(call("open",place,2).status==200);
    unsigned total=(output[0]<<8)|output[1];assert(total==size);
    for(unsigned offset=0;offset<total;offset+=512) {
        unsigned count=total-offset<512?total-offset:512;uint8_t read[]={offset>>8,offset,count>>8,count};
        assert(call("read",read,4).status==200 && !memcmp(output,encoded+offset,count));
    }
    before=writes;
    assert(call("nick",(uint8_t[]){1,TAG_SENSE_HF,0xc0,0x80},4).status==400 && writes==before);
    assert(call("nick",(uint8_t[]){1,TAG_SENSE_HF,'O','K'},4).status==200 && output[1]==STATUS_SUCCESS);
    assert(call("enable",(uint8_t[]){1,TAG_SENSE_HF,0},3).status==200 && !slotConfig.slots[1].enabled_hf);
    assert(call("activate",(uint8_t[]){1},1).status==200 && tag_emulation_get_slot()==1 && mode==DEVICE_MODE_TAG);
    assert(call("open",place,2).status==200);
    assert(call("clear",place,2).status==200 && output[1]==STATUS_SUCCESS);
    assert(call("read",(uint8_t[]){0,0,0,1},4).status==409);
    assert(slotConfig.slots[1].tag_hf==TAG_TYPE_UNDEFINED && slotConfig.slots[1].tag_lf==TAG_TYPE_VIKING);
    assert(!fds_is_exists(0x1101,2) && !fds_is_exists(0x1201,2) && fds_is_exists(0x1101,1));
    assert(!memcmp(lf_saved,records[index_of(0x1101,1)].data,4));
    before=writes;fail_at=writes+1;memset(lf_words,0x55,4);uint16_t previous_crc=lf_crc;
    assert(!tag_emulation_select_checked(0,(uint8_t *)image_words,sizeof(image_words)) && lf_crc==previous_crc && slotConfig.active_slot==1);
    fail_at=0;assert(tag_emulation_select_checked(0,(uint8_t *)image_words,sizeof(image_words)) && records[index_of(0x1101,1)].data[0]==0x55 && writes>before+1);
    // Missing, truncated and invalid images must not select or enable a place.
    seed(2,TAG_SENSE_HF,TAG_TYPE_MIFARE_1024,7);
    unsigned target=index_of(0x1102,2);uint16_t target_size=records[target].size;
    uint8_t prior_uid=((nfc_tag_mf1_information_t *)hf_words)->res_coll.uid[0];
    before=writes;records[target].size=0;
    assert(call("activate",(uint8_t[]){2},1).status==422 && writes==before && slotConfig.active_slot==0);
    slotConfig.slots[2].enabled_hf=false;
    assert(call("enable",(uint8_t[]){2,TAG_SENSE_HF,1},3).status==422 && !slotConfig.slots[2].enabled_hf);
    records[target].size=4;
    assert(call("activate",(uint8_t[]){2},1).status==422 && writes==before);
    records[target].size=target_size;
    assert(((nfc_tag_mf1_information_t *)hf_words)->res_coll.uid[0]==prior_uid && senses[1] && senses[2]);
    unsigned active_record=index_of(0x1100,2);uint16_t active_size=records[active_record].size;
    records[active_record].size=0;slotConfig.slots[0].enabled_hf=false;
    assert(call("enable",(uint8_t[]){0,TAG_SENSE_HF,1},3).status==422 && !slotConfig.slots[0].enabled_hf);
    records[active_record].size=active_size;slotConfig.slots[0].enabled_hf=true;
    // Exercise production loading, including a failure after the HF buffer changed.
    seed(2,TAG_SENSE_LF,TAG_TYPE_VIKING,9);
    records[index_of(0x1102,1)].size=0;
    memset(lf_words,0x66,8);lf_crc=123;hf_crc=456;
    uint8_t lf_before[sizeof(lf_words)];memcpy(lf_before,lf_words,sizeof(lf_words));
    assert(!tag_emulation_select_checked(2,(uint8_t *)image_words,sizeof(image_words)));
    assert(slotConfig.active_slot==0 && !memcmp(lf_before,lf_words,sizeof(lf_words)) && senses[1] && senses[2]);
    // A config write failure after both loads restores live shadow memory.
    seed(2,TAG_SENSE_LF,TAG_TYPE_VIKING,9);
    memset(lf_words,0x77,8);memcpy(lf_before,lf_words,sizeof(lf_words));
    // Source saves are now clean; fail the selection's config write.
    assert(save_data_by_tag_type(0,slotConfig.slots[0].tag_hf));
    assert(save_data_by_tag_type(0,slotConfig.slots[0].tag_lf));
    fail_at=writes+1;before=writes;
    assert(!tag_emulation_select_checked(2,(uint8_t *)image_words,sizeof(image_words)));
    assert(writes==before+1 && slotConfig.active_slot==0 && !memcmp(lf_before,lf_words,sizeof(lf_words)));
    assert(((nfc_tag_mf1_information_t *)hf_words)->res_coll.uid[0]==prior_uid && senses[1] && senses[2]);
    fail_at=0;
    // Shadow-mode bytes have no persistent copy: rollback must restore RAM and its dirty CRC.
    shadow_hf=true;((nfc_tag_mf1_information_t *)hf_words)->res_coll.uid[0]=0x88;
    uint16_t dirty_crc=hf_crc;fail_at=writes+1;
    assert(!tag_emulation_select_checked(2,(uint8_t *)image_words,sizeof(image_words)));
    assert(slotConfig.active_slot==0 && hf_crc==dirty_crc &&
        ((nfc_tag_mf1_information_t *)hf_words)->res_coll.uid[0]==0x88 && senses[1] && senses[2]);
    fail_at=0;shadow_hf=false;
    // Replacing the active HF place must not let its rollback backup overwrite the upload.
    size=make_upload();upload(size,0,TAG_SENSE_HF);
    assert(call("commit",(uint8_t[]){1,1,'x'},3).status==200 && output[1]==STATUS_SUCCESS);
    assert(slotConfig.slots[0].tag_hf==TAG_TYPE_MIFARE_4096 &&
        ((nfc_tag_mf1_information_t *)hf_words)->res_coll.uid[0]==0xab && senses[1] && senses[2]);
    // Stale controls and documents cannot mutate a newer image, even under a new lease.
    uint8_t stale_place[7]={2,TAG_SENSE_HF};revision_bytes(stale_place+2,2,TAG_SENSE_HF);stale_place[6]=1;
    uint8_t stale_activate[9]={2};revision_bytes(stale_activate+1,2,TAG_SENSE_HF);revision_bytes(stale_activate+5,2,TAG_SENSE_LF);
    uint8_t stale_begin[8]={2,TAG_SENSE_HF,0,9};memcpy(stale_begin+4,stale_place+2,4);
    records[target].data[target_size-1]^=1;before=writes;
    assert(raw_call("clear",stale_place,6).status==409);
    assert(raw_call("enable",stale_place,7).status==409);
    stale_place[6]='x';assert(raw_call("nick",stale_place,7).status==409);
    assert(raw_call("open",stale_place,6).status==409);
    assert(raw_call("activate",stale_activate,9).status==409);
    assert(raw_call("begin",stale_begin,8).status==409 && writes==before);
    // Recheck at commit as well as begin: legacy/native changes need no new handshake.
    size=make_upload();upload(size,1,TAG_SENSE_HF);records[index_of(0x1101,1)].data[0]^=1;
    // The other frequency may change independently without invalidating this target.
    assert(call("commit",(uint8_t[]){1,1,'x'},3).status==200 && output[1]==STATUS_SUCCESS);
    size=make_upload();upload(size,1,TAG_SENSE_HF);before=writes;
    records[index_of(0x1101,2)].data[20]^=1;
    assert(call("commit",(uint8_t[]){1,1,'x'},3).status==409 && writes==before);
    // A write on RF resume must pair the old snapshot with its old fingerprint.
    uint8_t guarded_open[6]={0,TAG_SENSE_HF};revision_bytes(guarded_open+2,0,TAG_SENSE_HF);
    rf_write_on_resume=true;
    assert(raw_call("open",guarded_open,6).status==200);
    uint32_t snapshot_revision=(uint32_t)output[2]<<24 | (uint32_t)output[3]<<16 | (uint32_t)output[4]<<8 | output[5];
    assert(snapshot_revision!=tag_emulation_place_revision(0,TAG_SENSE_HF));
    assert(!memcmp(guarded_open+2,output+2,4) && !rf_write_on_resume);
    // Active RAM changes also invalidate a document without waiting for a flash save.
    uint32_t live_revision=tag_emulation_place_revision(0,TAG_SENSE_HF);
    ((uint8_t *)hf_words)[300]^=1;
    assert(tag_emulation_place_revision(0,TAG_SENSE_HF)!=live_revision);
    uint8_t hf_before[32];memcpy(hf_before,hf_words,sizeof(hf_before));
    memset(image_words,0x55,sizeof(image_words));
    assert(slot_image_encode(TAG_TYPE_EM410X,(uint8_t *)image_words,sizeof(image_words),encoded,sizeof(encoded),&size));
    upload(size,0,TAG_SENSE_LF);
    assert(call("commit",(uint8_t[]){1,2,'L','F'},4).status==200 && output[1]==STATUS_SUCCESS);
    assert(slotConfig.slots[0].tag_lf==TAG_TYPE_EM410X && slotConfig.slots[0].tag_hf==TAG_TYPE_MIFARE_4096 &&
        !memcmp(hf_before,hf_words,sizeof(hf_before)) && ((uint8_t *)lf_words)[0]==0x55 && senses[1] && senses[2]);
    web_slots_reset();assert(call("read",(uint8_t[]){0,0,0,1},4).status==409);
    // Serial captures reuse the codec/storage, never select a slot or persist data.
    seed(0, TAG_SENSE_HF, TAG_TYPE_MIFARE_4096, 0x42);
    seed(1, TAG_SENSE_HF, TAG_TYPE_MF0UL11, 0x55);
    tag_emulation_load_data();
    tag_emulation_sense_run();
    before = writes;
    unsigned prior_loads = loads;
    uint16_t serial_size;
    uint8_t active[] = {0, TAG_SENSE_HF};
    uint8_t inactive[] = {1, TAG_SENSE_HF};
    g_is_tag_emulating = true;
    unchanged_hf = hf_sense_changes;
    assert(slot_snapshot_request(active, 2, output, sizeof(output), &serial_size) == STATUS_BUSY);
    snapshot_size = sizeof(decoded_words);
    assert(!tag_emulation_snapshot(0, TAG_SENSE_HF, (uint8_t *)decoded_words, &snapshot_size, &snapshot_type));
    assert(hf_sense_changes == unchanged_hf && critical_depth == 0 && senses[1] && senses[2]);
    g_is_tag_emulating = false;
    assert(slot_snapshot_request(active, 2, output, sizeof(output), &serial_size) == STATUS_SUCCESS);
    assert(serial_size == 6 && critical_depth == 0 && senses[1] && senses[2]);
    uint8_t token[4];
    memcpy(token, output, 4);
    size = (unsigned)output[4] << 8 | output[5];
    assert(size > 4096);
    assert(slot_image_encode(TAG_TYPE_MIFARE_4096, (uint8_t *)hf_words, sizeof(hf_words), encoded, sizeof(encoded), &df_size));
    assert(size == df_size);
    ((nfc_tag_mf1_information_t *)hf_words)->memory[0][0] ^= 0xff;
    for (unsigned offset = 0; offset < size;) {
        unsigned count = size - offset < sizeof(output) ? size - offset : sizeof(output);
        uint8_t request[8];
        memcpy(request, token, 4);
        request[4] = offset >> 8;
        request[5] = offset;
        request[6] = count >> 8;
        request[7] = count;
        assert(slot_snapshot_request(request, sizeof(request), output, sizeof(output), &serial_size) == STATUS_SUCCESS);
        assert(serial_size == count && !memcmp(output, encoded + offset, count));
        offset += count;
    }
    unchanged_hf = hf_sense_changes;
    assert(slot_snapshot_request(inactive, 2, output, sizeof(output), &serial_size) == STATUS_SUCCESS);
    assert(hf_sense_changes == unchanged_hf && writes == before && loads == prior_loads && tag_emulation_get_slot() == 0);
    uint8_t stale_read[8];
    memcpy(stale_read, token, 4);
    memcpy(stale_read + 4, (uint8_t[]){0, 0, 0, 1}, 4);
    assert(slot_snapshot_request(stale_read, 8, output, sizeof(output), &serial_size) == STATUS_BUSY);
    assert(slot_snapshot_request(token, 4, output, sizeof(output), &serial_size) == STATUS_BUSY);
    assert(slot_snapshot_request(inactive, 2, output, sizeof(output), &serial_size) == STATUS_SUCCESS);
    memcpy(token, output, 4);
    uint8_t invalid_read[8];
    memcpy(invalid_read, token, 4);
    memcpy(invalid_read + 4, (uint8_t[]){0xff, 0xff, 0, 1}, 4);
    assert(slot_snapshot_request(invalid_read, 8, output, sizeof(output), &serial_size) == STATUS_PAR_ERR);
    assert(slot_snapshot_request(token, 4, output, sizeof(output), &serial_size) == STATUS_SUCCESS && serial_size == 0);
    assert(slot_snapshot_request(token, 4, output, sizeof(output), &serial_size) == STATUS_BUSY);
    assert(slot_snapshot_request(active, 2, output, sizeof(output), &serial_size) == STATUS_SUCCESS);
    memcpy(stale_read, output, 4);
    web_slots_reset();
    assert(slot_snapshot_request(stale_read, 8, output, sizeof(output), &serial_size) == STATUS_BUSY);
    assert(writes == before && loads == prior_loads && senses[1] && senses[2] && mode == DEVICE_MODE_TAG);
    puts("Slot codecs, serial/web snapshots, upload ordering, save failures and opposite-frequency preservation passed");
}
