#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "st25tb_tearoff_stubs/nrf.h"
#include "../src/rfid/reader/hf/st25tb_tearoff.c"

mock_dwt_t mock_dwt;
volatile uint32_t hf_radio_tearoff_last_off_cycles;
static uint32_t reads[16], writes[16], delays[16];
static uint16_t read_status[16];
static bool verify_uids[16];
static unsigned read_count, read_index, write_count, reset_count, recovery_count,
                inventory_reset_count, feeds;
static bool write_ok, reset_ok, expired_radio;
static unsigned write_failures, reset_failures;
static uint32_t read_cost;
static unsigned progress_count;
static unsigned cancel_at_reads, cancel_at_resets;
static bool cancel_after_initial(void) { return progress_count != 0; }

static bool cancel_after_reads(void) { return read_index >= cancel_at_reads; }

static bool cancel_after_resets(void) { return reset_count >= cancel_at_resets; }

static void progress(uint8_t kind, uint32_t value, uint16_t delay_us) {
    assert(kind == ST25TB_TEAROFF_LOG_INITIAL && value == 0x10000 && delay_us == 0);
    progress_count++;
}

uint16_t st25tb_tearoff_read(const uint8_t uid[8], bool verify_uid,
        uint8_t block, uint8_t value[4], uint8_t *stage) {
    assert(uid[0] == 0xd0 && block == 5);
    *stage = 4;
    mock_dwt.CYCCNT += read_cost;
    assert(read_index < read_count);
    unsigned index = read_index++;
    verify_uids[index] = verify_uid;
    for (unsigned i = 0; i < 4; i++) value[i] = reads[index] >> (i * 8);
    return read_status[index];
}

bool st25tb_tearoff_write_selected(uint8_t block, const uint8_t value[4],
        uint32_t delay_us, uint32_t *cycles) {
    assert(block == 5 && delay_us <= 6000);
    if (write_count < 16) {
        delays[write_count] = delay_us;
        writes[write_count] = decode(value);
    }
    write_count++;
    mock_dwt.CYCCNT += 64000;
    *cycles = delay_us * 64;
    hf_radio_tearoff_last_off_cycles = 1000 * 64;
    if (write_failures) {
        write_failures--;
        return false;
    }
    return write_ok;
}

bool hf_radio_tearoff_reset(void) {
    reset_count++;
    mock_dwt.CYCCNT += 64000;
    hf_radio_tearoff_last_off_cycles = 1000 * 64;
    if (reset_failures) {
        reset_failures--;
        return false;
    }
    return reset_ok;
}

bool hf_radio_recover(void) {
    recovery_count++;
    mock_dwt.CYCCNT += 20u * 64000u;
    return reset_ok;
}

bool st25tb_reset_inventory(void) {
    inventory_reset_count++;
    mock_dwt.CYCCNT += 64000;
    return reset_ok;
}

bool hf_radio_expired(void) { return expired_radio; }

void bsp_delay_ms(uint16_t ms) { mock_dwt.CYCCNT += (uint32_t)ms * 64000; }

void bsp_wdt_feed(void) { feeds++; }

static void reset(void) {
    memset(reads, 0, sizeof(reads));
    memset(writes, 0, sizeof(writes));
    memset(delays, 0, sizeof(delays));
    memset(read_status, 0, sizeof(read_status));
    memset(verify_uids, 0, sizeof(verify_uids));
    mock_dwt.CYCCNT = 0;
    hf_radio_tearoff_last_off_cycles = 0;
    read_count = read_index = write_count = reset_count = recovery_count =
        inventory_reset_count = feeds = 0;
    write_ok = true;
    reset_ok = true;
    write_failures = reset_failures = 0;
    expired_radio = false;
    read_cost = 64000;
    progress_count = 0;
    cancel_at_reads = UINT_MAX;
    cancel_at_resets = UINT_MAX;
}

static st25tb_tearoff_request_t request(uint32_t duration) {
    static const uint8_t uid[8] = {0xd0, 2, 0x33, 1, 2, 3, 4, 5};
    return (st25tb_tearoff_request_t){.uid = uid, .block = 5, .target = 0xfffffffeu,
        .duration_ms = duration, .adjustment_us = ST25TB_TEAROFF_DEFAULT_ADJUSTMENT_US};
}

int main(void) {
    assert(ST25TB_TEAROFF_START_US == 130);
    assert(ST25TB_TEAROFF_DEFAULT_ADJUSTMENT_US == 22);
    assert(next_value(0xffffffffu, false, 0) == 0xfffffffeu);
    assert(next_value(0, false, 0) == 0);
    assert(next_value(0x10000u, false, 0) == 0xffffu);
    assert(next_value(0x10000u, true, 1) == 0xfffdu);
    assert(next_value(0x80000000u, false, 0) == 0x7fffffffu);

    reset();
    reads[0] = 0x10000;
    read_count = 1;
    st25tb_tearoff_request_t args = request(60000);
    args.cancelled = cancel_after_initial;
    args.progress = progress;
    st25tb_tearoff_result_t result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_CANCELLED && write_count == 0);
    assert(result.initial_valid && progress_count == 1);

    reset();
    reads[0] = reads[1] = 0xfffffffeu;
    read_count = 2;
    args = request(3000);
    args.reset_check = true;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && reset_count == 1);
    assert(result.initial_valid && result.initial_value == 0xfffffffeu);
    assert(write_count == 0 && read_index == 2);

    reset();
    reads[0] = reads[1] = 0xfffffffeu;
    read_count = 2;
    reset_failures = 1;
    args = request(3000);
    args.reset_check = true;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && reset_count == 2);

    reset();
    for (unsigned i = 0; i < 9; i++) reads[i] = 0xffffffffu;
    read_count = 9;
    args = request(10000);
    args.target = 0xffffffffu;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.value == args.target);
    assert(write_count == 0 && reset_count == 8 && read_index == 9);

    reset();
    reads[0] = 0xffffffffu;
    reads[1] = 0;
    read_count = 2;
    args = request(10000);
    args.target = 0xffffffffu;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_UNSUPPORTED && write_count == 0);

    reset();
    reads[0] = 0xfffffffeu;
    read_count = 1;
    reset_ok = false;
    args = request(3000);
    args.reset_check = true;
    args.cancelled = cancel_after_resets;
    cancel_at_resets = 1;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_CANCELLED && reset_count == 1 && write_count == 0);

    reset();
    reads[0] = 0xfffffffeu;
    for (unsigned i = 1; i < 4; i++) read_status[i] = STATUS_HF_TAG_NO;
    read_count = 4;
    args = request(3000);
    args.reset_check = true;
    args.cancelled = cancel_after_reads;
    cancel_at_reads = 4;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_CANCELLED && write_count == 0);

    reset();
    reads[0] = 0xfffffffc;
    reads[1] = 0xffffffff;
    for (unsigned i = 2; i < 11; i++) reads[i] = 0xfffffffe;
    read_count = 11;
    args = request(10000);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.valid);
    assert(result.value == args.target && write_count == 2 && reset_count == 8);
    assert(writes[1] == args.target && read_index == 11 && feeds);

    reset();
    reads[0] = 0x30000;
    reads[1] = 0x2ffff;
    reads[2] = 0x2ffff;
    read_count = 3;
    args = request(5);
    srand(1);
    uint32_t random_word = (uint32_t)rand();
    srand(1);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED);
    assert(writes[0] == 0x2ffff &&
           writes[1] == next_value(0x2ffff, true, random_word));

    reset();
    reads[0] = 0x30000;
    reads[1] = reads[2] = 0x20000;
    read_count = 3;
    args = request(5);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED);
    assert(writes[0] == 0x2ffff && writes[1] == 0x1ffff);

    reset();
    reads[0] = 0;
    read_count = 1;
    args = request(3000);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_UNSUPPORTED && write_count == 0);

    reset();
    reads[0] = 0xffffffff;
    reads[4] = 0xfffffffeu;
    for (unsigned i = 5; i < 13; i++) reads[i] = 0xfffffffeu;
    read_status[1] = STATUS_HF_ERR_STAT;
    read_status[2] = read_status[3] = STATUS_HF_TAG_NO;
    read_count = 13;
    args = request(10000);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.value == args.target);
    assert(write_count == 1 && reset_count == 8 && recovery_count == 1 &&
           inventory_reset_count == 1 &&
           delays[0] == 130 && read_index == 13);
    assert(verify_uids[0] && !verify_uids[1] && verify_uids[2] &&
           verify_uids[3] && verify_uids[4]);

    reset();
    reads[0] = reads[1] = 0xffffffff;
    read_count = 2;
    args = request(3);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED && write_count == 1);
    assert(delays[0] == 130);

    reset();
    reads[0] = 0xfffffffcu;
    reads[1] = reads[2] = reads[3] = 0xfffffffbu;
    read_count = 4;
    args = request(10000);
    args.target = 0xfffffffbu;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.value == args.target);
    assert(write_count == 1 && reset_count == 2 && read_index == 4);

    reset();
    reads[0] = 0xfffffffcu;
    reads[1] = reads[2] = 0xfffffffbu;
    reads[3] = 0xfffffffau;
    reads[4] = reads[5] = reads[6] = 0xfffffffbu;
    read_count = 7;
    args = request(3000);
    args.target = 0xfffffffbu;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.value == args.target);
    assert(write_count == 2 && reset_count == 4 && read_index == 7);

    reset();
    reads[0] = 0xffffffdeu;
    reads[1] = 0xffffffffu;
    reads[2] = 0xffffffdeu;
    for (unsigned i = 3; i < 12; i++) reads[i] = 0xffffffffu;
    read_count = 12;
    args = request(10000);
    args.target = 0xffffffffu;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.value == args.target);
    assert(write_count == 2 && reset_count == 9 && read_index == 12);

    reset();
    reads[0] = 0xffffffdeu;
    reads[1] = 0xffffffffu;
    reads[2] = 0;
    read_count = 3;
    args = request(3000);
    args.target = 0xffffffffu;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_UNSUPPORTED && write_count == 1);

    reset();
    reads[0] = 0xfffffffcu;
    reads[1] = reads[2] = reads[3] = 0xfffffffbu;
    read_status[2] = read_status[3] = read_status[4] = STATUS_HF_TAG_NO;
    read_count = 5;
    args = request(3000);
    args.target = 0xfffffffbu;
    args.cancelled = cancel_after_reads;
    cancel_at_reads = 5;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_CANCELLED && write_count == 1);

    reset();
    reads[0] = 0xfffffffcu;
    reads[1] = reads[2] = reads[3] = 0xfffffffbu;
    read_count = 4;
    reset_ok = false;
    args = request(3000);
    args.target = 0xfffffffbu;
    args.cancelled = cancel_after_resets;
    cancel_at_resets = 1;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_CANCELLED && reset_count == 1 && write_count == 1);

    reset();
    reads[0] = 0xfffffffcu;
    reads[1] = reads[2] = reads[3] = 0xfffffffbu;
    read_count = 4;
    read_cost = 170u * 64000u;
    args = request(500);
    args.target = 0xfffffffbu;
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED && write_count == 1);

    reset();
    reads[0] = 0xffffffff;
    reads[1] = 0xfffffffeu;
    for (unsigned i = 2; i < 10; i++) reads[i] = 0xfffffffeu;
    read_count = 10;
    write_ok = false;
    args = request(10000);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.value == args.target);
    assert(write_count == 1 && delays[0] == 130 && read_index == 10);
    assert(verify_uids[0] && verify_uids[1]);

    reset();
    reads[0] = reads[1] = 0xffffffff;
    reads[2] = 0xfffffffeu;
    for (unsigned i = 3; i < 11; i++) reads[i] = 0xfffffffeu;
    read_count = 11;
    write_failures = 1;
    args = request(10000);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_SUCCESS && result.value == args.target);
    assert(write_count == 2 && delays[0] == 130 && delays[1] == 130);
    assert(verify_uids[0] && verify_uids[1]);

    reset();
    reads[0] = 0xffffffff;
    read_count = 1;
    read_cost = 500u * 64000u;
    args = request(500);
    result = st25tb_tearoff_run(&args);
    assert(result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED && write_count == 0);

    reset();
    args = request(60000);
    run_t many = {.request = &args, .result = {.outcome = ST25TB_TEAROFF_UNSUPPORTED},
                  .last_check = mock_dwt.CYCCNT};
    for (unsigned i = 0; i < 5001; i++)
        assert(write_block(&many, 0xfffffffeu, 100));
    assert(write_count == 5001);
    mock_dwt.CYCCNT = 60000u * 64000u;
    assert(!write_block(&many, 0xfffffffeu, 100));
    assert(many.result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED && write_count == 5001);

    reset();
    args = request(120000);
    mock_dwt.CYCCNT = UINT32_MAX - 1000;
    run_t long_run = {.request = &args, .last_check = mock_dwt.CYCCNT};
    for (unsigned minute = 0; minute < 3; minute++) {
        mock_dwt.CYCCNT += 30000u * 64000u;
        assert(!expired(&long_run));
    }
    mock_dwt.CYCCNT += 30000u * 64000u;
    assert(expired(&long_run));
    assert(long_run.result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED);

    puts("ST25TB tearoff traces passed");
}
