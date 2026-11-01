#include "hf_reader.h"
#include "hf_radio.h"
#include "iso15693/frame.h"
#include "app_status.h"
#include "data_cmd.h"
#include "nrf.h"
#include "st25tb_tearoff.h"
#include "st25tb_reader.h"
#include "utils/dataframe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

test_dwt_t test_dwt;
test_core_t test_core;
volatile bool m_is_field_on;
static bool mode = true, capture, vicinity, open_ok = true, deep, kept;
static bool reject_deep, reject_shallow, wrong_select, reset_needed;
static unsigned inventory_resets, first_crc;
static unsigned transient_block = 999, transient_status, block_attempts;
static bool multiple_unsupported, multiple_truncated;
static unsigned single_reads, multiple_reads, geometry_blocks = 28;
static unsigned opens, closes, exchanges, fail_block = 999, step = 1000;
static const uint8_t uid_v[] = {0xe0,4,1,0x10,3,4,5,6};
static uint8_t uid_b[] = {0xd0,2,0x33,5,4,3,2,1};
static bool write_test, write_ok, write_verify, write_applied;
static unsigned writes;
static unsigned privacy_requests, privacy_auths, vicinity_writes;
static bool privacy_reject, vicinity_write, verify_wrong, security_unsupported;
static uint8_t vicinity_value[4];
static uint8_t write_current[4], write_frame[ST25TB_WRITE_FRAME_BYTES];
int get_device_mode(void) { return mode ? 1 : 0; }
bool nfca_session_active(void) { return capture; }
bool hf_radio_open(bool v) {
    vicinity = v;
    if (!kept) {
        opens++;
    }
    kept = false;
    return open_ok;
}
bool hf_radio_close(void) { kept = false; closes++; return true; }
void hf_radio_keep(void) { kept = true; }
bool hf_radio_release(void) { return !kept || hf_radio_close(); }
void hf_radio_depth(bool deeper) { deep = deeper; }

bool hf_radio_send(const uint8_t *data, size_t length) {
    assert(length == 1 && data[0] == 0x0c);
    inventory_resets++;
    reset_needed = false;
    return true;
}

bool hf_radio_st25tb_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES]) {
    assert(write_test && frame[0] == 9);
    writes++;
    memcpy(write_frame, frame, sizeof(write_frame));
    write_applied = write_ok && write_verify;
    return write_ok;
}

bool hf_radio_expired(void) { return test_dwt.CYCCNT >= HF_READER_DEADLINE_CYCLES; }

void bsp_wdt_feed(void) {}
data_source_t data_frame_source(void) { return DATA_SOURCE_USB; }
bool advertising_stop_checked(void) { return false; }
void advertising_start(bool erase_bonds) { (void)erase_bonds; assert(false); }
bool is_usb_working(void) { return true; }
bool usb_hfclk_active(void) { return true; }
bool web_tearoff_running(void) { return false; }
bool web_tearoff_cancelled(void) { assert(false); return false; }
void web_tearoff_service(void) { assert(false); }
void web_tearoff_progress(uint8_t kind, uint32_t value, uint16_t delay_us) {
    (void)kind; (void)value; (void)delay_us; assert(false);
}
bool usb_cdc_tx_ready(void) { return true; }
void usb_cdc_write(const void *data, uint16_t length) { (void)data; (void)length; assert(false); }
void usb_poll(void) {}
void usb_cdc_tearoff_begin(void) { assert(false); }
void usb_cdc_tearoff_end(void) { assert(false); }
bool usb_cdc_tearoff_cancelled(void) { return false; }
data_frame_tx_t *data_frame_make(uint16_t command, uint16_t status, uint16_t length, uint8_t *data) {
    (void)command; (void)status; (void)length; (void)data; assert(false); return NULL;
}

void hf_radio_tearoff_deadline(uint32_t ms) { (void)ms; assert(false); }
void hf_radio_tearoff_timing(bool enabled) { (void)enabled; assert(false); }
hf_tearoff_timing_t hf_radio_tearoff_timing_result(void) {
    assert(false); return (hf_tearoff_timing_t){0};
}
st25tb_tearoff_result_t st25tb_tearoff_run(const st25tb_tearoff_request_t *request) {
    (void)request; assert(false); return (st25tb_tearoff_result_t){0};
}

bool hf_radio_tearoff_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES], uint32_t delay, uint32_t *cycles) {
    (void)frame; (void)delay; (void)cycles; assert(false); return false;
}

uint16_t hf_radio_exchange(const uint8_t *data, size_t length,
        size_t expected, uint8_t *out, size_t capacity, size_t *received) {
    assert(capacity >= 24);
    memset(out, 0, 24);
    exchanges++;
    test_dwt.CYCCNT += step;
    *received = 0;
    if (!vicinity) {
        if (data[0] == 6) {
            if (first_crc) { if (first_crc++ == 1) return STATUS_HF_ERR_CRC; return STATUS_HF_TAG_NO; }
            if (reset_needed || (reject_deep && deep) || (reject_shallow && !deep))
                return STATUS_HF_TAG_NO;
            out[0] = 0x5a;
        } else if (data[0] == 0x0e) {
            out[0] = wrong_select ? 0x5b : 0x5a;
            reset_needed = wrong_select;
            wrong_select = false;
        }
        else if (data[0] == 0x0b)
            for (unsigned i=0;i<8;i++) out[i] = uid_b[7-i];
        else if (data[0] == 8) {
            block_attempts++;
            if (data[1] == transient_block) {
                transient_block = 999;
                return transient_status;
            }
            if (data[1] == fail_block) return STATUS_HF_ERR_CRC;
            if (write_test) memcpy(out, write_applied ? write_frame + 2 : write_current, 4);
            else memset(out, data[1], 4);
        } else if (data[0] == 5) out[0] = 0x50;
        *received = expected;
        return 0;
    }
    if (data[0] == 2) {
        assert(length == 3 || length == 8);
        assert(data[2] == 4);
        if (data[1] == 0xb2) {
            privacy_requests++;
            out[1] = 0x12;
            out[2] = 0x34;
            *received = 5;
        } else {
            const uint8_t masked[] = {2, 0xb3, 4, 4, 0x13, 0x16, 0x57, 0x53};
            assert(length == 8 && !memcmp(data, masked, 8));
            privacy_auths++;
            if (privacy_reject) {
                return STATUS_HF_TAG_NO;
            }
            *received = 3;
        }
        iso15693_append_crc(out, *received - 2);
        return 0;
    }
    assert(data[0] == 0x22 || data[0] == 0x26 || data[0] == 0x62);
    if (data[1] != 1) assert(iso15693_wire_uid_matches(data + 2, uid_v));
    if (data[1] == 0x2b) {
        assert(length == 10);
        out[1] = 15;
        iso15693_uid_to_wire(out+2, uid_v);
        out[10]=0x12; out[11]=0x34; out[12]=geometry_blocks-1; out[13]=3; out[14]=1;
        *received = 17;
    } else if (data[1] == 0x23) {
        assert(length == 12);
        unsigned count = data[11]+1;
        assert(count > 1 && count <= 5 && data[10]+count <= geometry_blocks);
        multiple_reads++;
        if (multiple_unsupported) {
            out[0]=1;out[1]=1;*received=4;
        } else {
            if (fail_block >= data[10] && fail_block < data[10]+count) return STATUS_HF_ERR_CRC;
            for(unsigned i=0;i<count;i++)memset(out+1+4*i,data[10]+i,4);
            *received=count*4+3-(multiple_truncated ? 4 : 0);
        }
    } else if (data[1] == 0x20) {
        single_reads++;
        assert(length == 11);
        if (data[10] == fail_block) return STATUS_HF_ERR_CRC;
        if (vicinity_write) {
            memcpy(out + 1, vicinity_value, 4);
            if (verify_wrong && vicinity_writes >= 2) {
                out[1] ^= 1;
            }
        } else {
            memset(out + 1, data[10], 4);
        }
        if (data[0] == 0x62) {
            memmove(out + 2, out + 1, 4);
            out[1] = data[10] & 1;
            *received = 8;
        } else {
            *received = 7;
        }
    } else if (data[1] == 0x21) {
        assert(vicinity_write && length == 15);
        vicinity_writes++;
        memcpy(vicinity_value, data + 11, 4);
        *received = 3;
    } else if (data[1] == 0x2c) {
        assert(length == 12);
        unsigned count = data[11]+1;
        if (security_unsupported) {
            out[0] = 1;
            out[1] = 1;
            *received = 4;
            iso15693_append_crc(out, 2);
            return 0;
        }
        for (unsigned i=0;i<count;i++) out[i+1]=(data[10]+i)&1;
        *received = count+3;
    } else {
        assert(data[1] == 1 && length == 3);
        out[1]=0x12;
        iso15693_uid_to_wire(out+2,uid_v);
        *received=12;
    }
    iso15693_append_crc(out, *received-2);
    return 0;
}

static void check_st25tb_write(uint8_t block, const uint8_t expected[4],
        const uint8_t value[4], uint16_t expected_status, unsigned expected_writes) {
    uint8_t request[17], output[HF_READER_OUTPUT_BYTES];
    memcpy(request, uid_b, 8);
    request[8] = block;
    memcpy(request + 9, expected, 4);
    memcpy(request + 13, value, 4);
    write_applied = false;
    test_dwt.CYCCNT = 0;
    unsigned previous_writes = writes;
    uint16_t size;
    assert(hf_reader_command(DATA_CMD_ST25TB_WRITE_BLOCK, sizeof(request), request,
                            output, sizeof(output), &size) == expected_status);
    assert(opens == closes && writes == previous_writes + expected_writes);
    if (!expected_writes) {
        assert(size == 0);
        return;
    }
    assert(write_frame[1] == block && !memcmp(write_frame + 2, value, 4));
    if (!write_ok) {
        assert(size == 0);
        return;
    }
    assert(size == 4);
    assert(!memcmp(output, write_applied ? value : write_current, 4));
}

static void test_st25tb_writes(void) {
    const uint8_t current[4] = {0x10, 0x20, 0x30, 0x40};
    const uint8_t changed[4] = {0x11, 0x22, 0x33, 0x44};
    write_test = write_ok = write_verify = true;
    memcpy(write_current, current, 4);

    check_st25tb_write(7, changed, changed, STATUS_HF_ERR_STAT, 0);
    uid_b[2] = 0x18;
    check_st25tb_write(7, current, changed, STATUS_NOT_IMPLEMENTED, 0);
    uid_b[2] = 0x1f;
    check_st25tb_write(7, current, changed, STATUS_NOT_IMPLEMENTED, 0);
    uid_b[2] = 0x33;

    /* An increase must be rejected for either counter block. */
    for (uint8_t block = 5; block <= 6; block++)
        check_st25tb_write(block, current, changed, STATUS_PAR_ERR, 0);
    /* Counter words compare in little-endian order, rather than byte order. */
    const uint8_t decreased[4] = {0xff, 0xff, 0xff, 0x3f};
    for (uint8_t block = 5; block <= 6; block++)
        check_st25tb_write(block, current, decreased, STATUS_HF_TAG_OK, 1);

    /* OTP may clear bits, but may not set previously cleared bits. */
    check_st25tb_write(255, current, changed, STATUS_PAR_ERR, 0);
    const uint8_t cleared[4] = {0x00, 0x20, 0x10, 0x40};
    check_st25tb_write(255, current, cleared, STATUS_HF_TAG_OK, 1);

    write_ok = false;
    check_st25tb_write(7, current, changed, STATUS_HF_ERR_STAT, 1);
    write_ok = true;
    write_verify = false;
    check_st25tb_write(7, current, changed, STATUS_HF_ERR_STAT, 1);
    write_verify = true;
    check_st25tb_write(7, current, changed, STATUS_HF_TAG_OK, 1);
    write_test = false;
}

static void test_vicinity_operations(void) {
    uint8_t output[HF_READER_OUTPUT_BYTES];
    uint16_t size;
    const uint8_t password[] = {1, 0x22, 0x45, 0x67};
    test_dwt.CYCCNT = 0;
    assert(hf_reader_command(DATA_CMD_SLIX_PRIVACY_SCAN, 4, password, output, sizeof(output), &size) == 0);
    assert(size == 12 && privacy_requests == 1 && privacy_auths == 1);
    privacy_reject = true;
    assert(hf_reader_command(DATA_CMD_SLIX_PRIVACY_SCAN, 4, password, output, sizeof(output), &size) == STATUS_MF_ERR_AUTH);
    assert(size == 0 && privacy_requests == 2 && privacy_auths == 2);
    privacy_reject = false;
    uint8_t request[21] = {0};
    memcpy(request, uid_v, 8);
    request[8] = 2;
    memcpy(request + 13, (const uint8_t[]){1, 2, 3, 4}, 4);
    memcpy(request + 17, password, 4);
    vicinity_write = true;
    assert(hf_reader_command(DATA_CMD_HF15_WRITE_BLOCK, 21, request, output, sizeof(output), &size) == 0);
    assert(size == 4 && !memcmp(output, request + 13, 4) && vicinity_writes == 1);
    assert(privacy_requests == 3 && privacy_auths == 3);
    assert(hf_reader_command(DATA_CMD_HF15_WRITE_BLOCK, 17, request, output, sizeof(output), &size) == STATUS_CMD_ERR);
    request[8] = 3;
    assert(hf_reader_command(DATA_CMD_HF15_WRITE_BLOCK, 17, request, output, sizeof(output), &size) == STATUS_CMD_ERR);
    request[8] = geometry_blocks;
    assert(hf_reader_command(DATA_CMD_HF15_WRITE_BLOCK, 17, request, output, sizeof(output), &size) == STATUS_PAR_ERR);
    assert(vicinity_writes == 1);
    request[8] = 4;
    memcpy(request + 9, vicinity_value, 4);
    request[13] = 9;
    verify_wrong = true;
    assert(hf_reader_command(DATA_CMD_HF15_WRITE_BLOCK, 17, request, output, sizeof(output), &size) == STATUS_HF_ERR_STAT);
    assert(size == 0 && vicinity_writes == 2);
    verify_wrong = false;
    vicinity_write = false;
    security_unsupported = true;
    uint8_t dump[11];
    memcpy(dump, uid_v, 8);
    dump[8] = 0;
    dump[9] = geometry_blocks;
    dump[10] = 4;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP, 11, dump, output, sizeof(output), &size) == 0);
    for (unsigned block = 0; block < geometry_blocks; block++) {
        assert(output[25 + block * 6] == 1 + (block & 1));
    }
    security_unsupported = false;
    const uint8_t raw[] = {2, 3, 0x26, 1, 0, 3, 0x26, 1, 0};
    unsigned before = exchanges;
    assert(hf_reader_command(DATA_CMD_HF15_RAW, sizeof(raw), raw, output, sizeof(output), &size) == 0);
    assert(output[0] == 2 && size == 31 && exchanges == before + 2);
    uint8_t retained_raw[sizeof(raw)];
    memcpy(retained_raw, raw, sizeof(raw));
    retained_raw[0] |= NFCV_RAW_KEEP_FIELD;
    assert(hf_reader_command(DATA_CMD_HF15_RAW, sizeof(retained_raw), retained_raw,
                            output, sizeof(output), &size) == 0);
    assert(kept && opens == closes + 1 && output[0] == 2);
    before = opens;
    assert(hf_reader_command(DATA_CMD_HF15_RAW, sizeof(raw), raw,
                            output, sizeof(output), &size) == 0);
    assert(!kept && opens == before && opens == closes);
    assert(hf_reader_command(DATA_CMD_HF15_RAW, sizeof(retained_raw), retained_raw,
                            output, sizeof(output), &size) == 0);
    assert(kept && hf_radio_release() && opens == closes);
    const uint8_t failed_raw[] = {NFCV_RAW_KEEP_FIELD | 1, 11, 0x22, 0x20,
        6, 5, 4, 3, 0x10, 1, 4, 0xe0, 4};
    fail_block = 4;
    assert(hf_reader_command(DATA_CMD_HF15_RAW, sizeof(failed_raw), failed_raw,
                            output, sizeof(output), &size) == 0);
    assert(!kept && opens == closes && output[0] == 1);
    assert(output[1] == 0 && output[2] == STATUS_HF_ERR_CRC);
    fail_block = 999;
    const uint8_t malformed[] = {2, 3, 0x26, 1, 0, 22};
    before = exchanges;
    assert(hf_reader_command(DATA_CMD_HF15_RAW, sizeof(malformed), malformed, output, sizeof(output), &size) == STATUS_PAR_ERR);
    assert(exchanges == before && opens == closes);
}

int main(void) {
    uint8_t request[11], output[HF_READER_OUTPUT_BYTES];
    uint16_t size;
    memcpy(request,uid_v,8); request[8]=0;request[9]=28;request[10]=4;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,10,request,output,sizeof(output),&size)==STATUS_PAR_ERR);
    assert(opens==0);
    mode=false;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==STATUS_DEVICE_MODE_ERROR);
    mode=true;capture=true;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==STATUS_BUSY);
    capture=false;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==0);
    assert(size==192 && output[0]==1 && output[1]==3 && output[2]==0);
    assert(!memcmp(output+4,uid_v,8) && output[15]==7 && output[16]==0x12 && output[17]==0x34);
    for (unsigned i=0;i<28;i++) {
        assert(output[24+i*6]==1 && output[25+i*6]==1+(i&1));
        assert(output[26+i*6]==i);
    }
    assert(opens==1 && closes==1 && multiple_reads==6 && single_reads==0);
    multiple_unsupported=true;test_dwt.CYCCNT=0;multiple_reads=single_reads=0;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==0);
    assert(output[2]==0 && multiple_reads==1 && single_reads==28);
    multiple_unsupported=false;test_dwt.CYCCNT=0;multiple_reads=single_reads=0;
    multiple_truncated=true;test_dwt.CYCCNT=0;multiple_reads=single_reads=0;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==0);
    assert(output[2]==1 && output[3]==STATUS_HF_ERR_STAT && output[24]==0);
    assert(multiple_reads==1 && single_reads==0);
    multiple_truncated=false;multiple_reads=single_reads=0;
    fail_block=5; test_dwt.CYCCNT=0;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==0);
    assert(output[2]==1 && output[3]==STATUS_HF_ERR_CRC);
    assert(output[24+4*6]==1 && output[24+5*6]==0 && multiple_reads==2 && single_reads==0);
    fail_block=999;step=HF_READER_DEADLINE_CYCLES/4;test_dwt.CYCCNT=0;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==0);
    assert(output[2]==2 && output[24]==1 && output[24+14*6]==1 && output[24+15*6]==0);
    step=1000;test_dwt.CYCCNT=0;
    geometry_blocks=64;request[9]=64;multiple_reads=single_reads=0;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==0);
    assert(size==HF_READER_OUTPUT_BYTES && output[2]==0 && multiple_reads==13 && single_reads==0);
    assert(output[24+63*6]==1 && output[26+63*6]==63);
    geometry_blocks=8;request[9]=8;test_dwt.CYCCNT=0;multiple_reads=single_reads=0;
    assert(hf_reader_command(DATA_CMD_HF15_DUMP,11,request,output,sizeof(output),&size)==0);
    assert(output[2]==0 && multiple_reads==0 && single_reads==8);
    assert(hf_reader_command(DATA_CMD_ST25TB_DUMP,8,uid_b,output,sizeof(output),&size)==0);
    assert(size==120 && output[1]==2 && output[2]==0 && output[15]==8 && output[19]==0x5a);
    assert(!memcmp(output+4,uid_b,8) && output[20]==255 && output[26+15*6]==15);
    for (unsigned product=0x18;product<=0x1b;product++) {
        uid_b[2]=product;
        assert(hf_reader_command(DATA_CMD_ST25TB_INFO,0,NULL,output,sizeof(output),&size)==0);
        assert(size==11 && !memcmp(output,uid_b,8) && output[9]==16 && output[10]==4);
        memcpy(request,uid_b,8);request[8]=255;
        assert(hf_reader_command(DATA_CMD_ST25TB_READ_BLOCK,9,request,output,sizeof(output),&size)==0);
        assert(size==4 && output[0]==255);
        assert(hf_reader_command(DATA_CMD_ST25TB_DUMP,8,uid_b,output,sizeof(output),&size)==0);
        assert(size==120 && output[2]==0 && output[15]==8 && output[20]==255);
        for(unsigned i=0;i<16;i++)assert(output[24+i*6]==1 && output[26+i*6]==i);
    }
    uid_b[2]=0x33;
    memcpy(request,uid_b,8);request[8]=5;
    for (unsigned error=STATUS_HF_TAG_NO;error<=STATUS_HF_ERR_CRC;error++) {
        unsigned before_blocks=block_attempts;
        transient_status=error;transient_block=5;
        assert(hf_reader_command(DATA_CMD_ST25TB_READ_BLOCK,9,request,output,sizeof(output),&size)==0);
        assert(size==4 && output[0]==5 && block_attempts==before_blocks+2);
        before_blocks=block_attempts;transient_block=5;
        assert(hf_reader_command(DATA_CMD_ST25TB_DUMP,8,uid_b,output,sizeof(output),&size)==0);
        assert(size==120 && output[2]==0 && block_attempts==before_blocks+18);
    }
    fail_block=5;
    unsigned before_blocks=block_attempts;
    assert(hf_reader_command(DATA_CMD_ST25TB_DUMP,8,uid_b,output,sizeof(output),&size)==0);
    assert(output[2]==1 && output[3]==STATUS_HF_ERR_CRC && output[24+4*6]==1 && output[24+5*6]==0);
    assert(block_attempts==before_blocks+7);
    fail_block=999;
    test_st25tb_writes();
    /* Tearoff reads retain the modulation learned earlier in the command. */
    uint8_t value[4], stage;
    unsigned before=exchanges, before_resets=inventory_resets;
    deep=false;reject_shallow=true;
    assert(st25tb_tearoff_read(uid_b,true,5,value,&stage)==0);
    assert(deep && exchanges==before+5 && stage==4 && value[0]==5);
    before=exchanges;
    assert(st25tb_tearoff_read(uid_b,true,5,value,&stage)==0);
    assert(deep && exchanges==before+4 && stage==4 && value[0]==5);
    assert(inventory_resets==before_resets);
    reject_shallow=false;
    unsigned saved_resets=inventory_resets;
    reject_deep=true;
    assert(hf_reader_command(DATA_CMD_ST25TB_INFO,0,NULL,output,sizeof(output),&size)==0);
    assert(!deep && size==11 && !memcmp(output,uid_b,8));
    assert(inventory_resets==saved_resets+1);
    reject_deep=false;wrong_select=true;
    assert(hf_reader_command(DATA_CMD_ST25TB_INFO,0,NULL,output,sizeof(output),&size)==0);
    assert(!deep && !reset_needed && size==11 && !memcmp(output,uid_b,8));
    assert(inventory_resets==saved_resets+2);
    first_crc=1;
    assert(hf_reader_command(DATA_CMD_ST25TB_INFO,0,NULL,output,sizeof(output),&size)==STATUS_HF_ERR_CRC);
    first_crc=0;
    uid_b[2]=0x1f;
    assert(hf_reader_command(DATA_CMD_ST25TB_INFO,0,NULL,output,sizeof(output),&size)==0);
    assert(output[9]==0);
    assert(hf_reader_command(DATA_CMD_ST25TB_DUMP,8,uid_b,output,sizeof(output),&size)==STATUS_NOT_IMPLEMENTED);
    assert(opens==closes && exchanges>16);
    test_vicinity_operations();
    open_ok=false;
    assert(hf_reader_command(DATA_CMD_ST25TB_INFO,0,NULL,output,sizeof(output),&size)==STATUS_HF_ERR_STAT);
    assert(opens==closes && size==0);
    puts("Active readers: dumps, guarded writes, deadlines, identity, locks and ownership passed");
}
