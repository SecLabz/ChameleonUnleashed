#include "hf_radio.h"
#include "hf_reader.h"
#include "rc522_raw.h"
#include "hf_iq.h"
#include "nfcb_decode.h"
#include "nfcv_reader_codec.h"
#include "iso15693/frame.h"
#include "rf_timeslot.h"
#include "hfclk.h"
#include "app_status.h"
#include "nrf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

test_dwt_t test_dwt;
test_debug_t test_debug;
volatile bool g_is_ble_connected;
extern volatile uint32_t hf_radio_tearoff_last_delta_cycles;
volatile uint32_t hf_radio_tearoff_last_off_cycles;
static uint8_t samples[11265], last_frame[64];
static bool ready, open_ok = true, tx_ok = true, usb_clock = true, frontend_clock = true;
static bool vicinity, drop_clock, bad_crc, skip_slot;
static bool short_slots_only, inside_slot, dma_active;
static uint8_t clock_owners;
static unsigned clock_acquires, clock_releases;
static uint32_t acquire_error, wait_error, release_error;
uint8_t app_hfclk_owners(void) { return clock_owners; }
uint32_t app_hfclk_acquire(uint8_t owner) {
    assert(owner == HFCLK_READER && !inside_slot);
    clock_acquires++;
    if (!acquire_error) clock_owners |= owner;
    return acquire_error;
}
uint32_t app_hfclk_release(uint8_t owner) {
    assert(owner == HFCLK_READER && !inside_slot && !dma_active);
    clock_releases++;
    if (!release_error) clock_owners &= ~owner;
    return release_error;
}
uint32_t app_hfclk_wait(void) { assert(!inside_slot); return wait_error; }
static bool last_hardware_tx, depth, live_fail, last_stop, drop_live_clock;
static unsigned live_decodes, finishes;
static unsigned opens, closes, acquisitions, recoveries, invalidations;
static unsigned exchanges, decodes, slots, sends, writes, cycle_takes;
static size_t expected_reply, last_length;
static unsigned expected_samples;
static uint32_t expected_slot, pending_write_cycles;
static const uint8_t request[] = {2,0};

void bsp_wdt_feed(void) {}

uint8_t *hf_iq_prepare(bool v) { vicinity = v; return samples; }
bool usb_hfclk_active(void) { return usb_clock; }
bool rc522_raw_clock_ready(void) { return frontend_clock; }
bool rc522_raw_ready(void) { return ready; }
void rc522_raw_invalidate(void) { invalidations++; ready = false; }
bool rc522_raw_open(rc522_raw_protocol_t protocol) {
    opens++;
    assert(protocol == (vicinity ? RC522_RAW_NFCV : RC522_RAW_NFCB));
    ready = open_ok;
    return ready;
}

bool rc522_raw_close(void) { closes++; ready = false; return true; }
bool rc522_raw_acquire_tx(void) { acquisitions++; return tx_ok; }
bool rc522_raw_recover(void) { recoveries++; ready = true; return true; }
void rc522_raw_depth(bool deeper) { depth = deeper; }

bool rf_timeslot_run(uint32_t microseconds, bool high, bool extend, void (*work)(void)) {
    slots++;
    assert((!expected_slot || microseconds == expected_slot) && high == !g_is_ble_connected && !extend);
    if (short_slots_only && microseconds > 10000) return false;
    if (skip_slot) return false;
    inside_slot = true;
    work();
    inside_slot = false;
    return true;
}

uint32_t rf_timeslot_last_run_cycles(void) { return 128000; } /* 2 ms at 64 MHz. */
uint32_t rf_timeslot_last_grant_wait_cycles(void) { return 64000; } /* 1 ms. */

uint16_t nfcb_crc_b(const uint8_t *data, size_t size) {
    assert(data && (size == 1 || size == 2 || size == 6));
    if (size == 2) assert(!memcmp(data, request, sizeof(request)));
    return 0x3412; /* Independent framing vector, not a decoder implementation. */
}

void iso15693_append_crc(uint8_t *data, uint16_t size) {
    if (size == 2) {
        assert(!memcmp(data, request, sizeof(request)));
    } else {
        assert(size == 15 || size == 22);
        for (unsigned index = 0; index < size; index++) {
            assert(data[index] == 0x42);
        }
    }
    data[size] = 0xde;
    data[size + 1] = 0xad;
}

size_t nfcv_reader_encode(const uint8_t *data, size_t size, uint8_t *levels, size_t capacity) {
    const uint8_t frame[] = {2,0,0xde,0xad};
    assert(capacity >= 3);
    if (size == sizeof(frame)) {
        assert(!memcmp(data, frame, sizeof(frame)));
    } else {
        assert((size == 17 || size == 24) && data[size - 2] == 0xde && data[size - 1] == 0xad);
    }
    levels[0] = 1; levels[1] = 0; levels[2] = 1;
    return 3;
}

static void capture(const rc522_raw_transfer_t *transfer) {
    assert(transfer->samples == samples && transfer->sample_count == expected_samples);
    assert(transfer->length <= sizeof(last_frame));
    assert(transfer->live == (transfer->hardware_tx || vicinity));
    last_hardware_tx = transfer->hardware_tx;
    last_length = transfer->length;
    memcpy(last_frame, transfer->data, last_length);
}

void rc522_raw_exchange(const rc522_raw_transfer_t *transfer) {
    capture(transfer);
    if (vicinity) {
        assert(inside_slot && (clock_owners & HFCLK_READER));
        dma_active = true;
    }
    exchanges++;
    if (drop_clock) frontend_clock = false;
    if (transfer->dma_cycles) *transfer->dma_cycles += 32000; /* 500 us. */
}

void rc522_raw_send(const rc522_raw_transfer_t *transfer) {
    last_length = transfer->length;
    memcpy(last_frame, transfer->data, last_length);
    sends++;
}

void rc522_raw_write(const rc522_raw_transfer_t *transfer) {
    assert(transfer->length == 8 && !transfer->hardware_tx);
    writes++;
}

void rc522_raw_tearoff(const rc522_raw_transfer_t *transfer, uint32_t delay_us) {
    assert(transfer->length == 8 && !transfer->hardware_tx && delay_us == 100);
    pending_write_cycles = 6400; /* Stubbed event completion, no RF waveform. */
    writes++;
}

void rc522_raw_reset(void) { }
uint32_t rc522_raw_take_write_cycles(void) {
    uint32_t result = pending_write_cycles;
    pending_write_cycles = 0;
    cycle_takes++;
    return result;
}

bool nfcb_decode_iq_checked(const uint8_t *data, size_t count, size_t expected,
        uint8_t *output, unsigned *offset, bool *crc_error) {
    assert(data == samples + 1 && count == expected_samples && expected == expected_reply);
    decodes++;
    *offset = 0;
    *crc_error = bad_crc;
    if (bad_crc) return false;
    memset(output, 0x5a, expected);
    return true;
}

unsigned rc522_raw_available(void) { return expected_samples; }
void rc522_raw_finish(bool stop) {
    assert(!inside_slot);
    if (vicinity) assert(!stop && dma_active && (clock_owners & HFCLK_READER));
    dma_active = false;
    finishes++; last_stop = stop;
}

bool nfcb_decode_iq_live(const uint8_t *data, size_t count, size_t expected,
        uint8_t *output, unsigned *offset, unsigned (*available)(void)) {
    assert(data == samples + 1 && count == expected_samples && expected == expected_reply);
    assert(available == rc522_raw_available);
    live_decodes++;
    if (drop_live_clock) frontend_clock = false;
    *offset = 0;
    if (bad_crc || live_fail) return false;
    memset(output, 0x5a, expected);
    return true;
}

size_t nfcv_reader_decode_iq_checked(const uint8_t *data, size_t count, uint8_t *output,
        size_t capacity, unsigned *offset, bool *crc_error) {
    assert(data == samples + 1 && count == 11264 && capacity == 24);
    assert(!inside_slot && !dma_active && (clock_owners & HFCLK_READER));
    decodes++;
    *offset = 0; *crc_error = bad_crc;
    if (bad_crc) return 0;
    memset(output, 0x5a, 5);
    return 5;
}

static uint16_t exchange(size_t reply_size, unsigned capture_samples, uint32_t slot_us) {
    uint8_t output[24];
    size_t received = 123;
    expected_reply = reply_size;
    expected_samples = capture_samples;
    expected_slot = slot_us;
    uint16_t status = hf_radio_exchange(request, sizeof(request), reply_size,
        output, sizeof(output), &received);
    if (status == STATUS_HF_TAG_OK) assert(received == (vicinity ? 5 : reply_size));
    else assert(received == 0);
    return status;
}

static void profiles(void) {
    tx_ok = false;
    assert(hf_radio_open(false) && acquisitions == 1);
    assert(exchange(3, 2048, 15000) == STATUS_HF_TAG_OK && !last_hardware_tx);
    assert(hf_radio_close() && closes == 1);
    open_ok = false;
    assert(!hf_radio_open(false) && acquisitions == 1);
    assert(hf_radio_close() && closes == 2);
    assert(hf_radio_open(true) == false);
    assert(hf_radio_close() && opens == 3);
}

static void direct(void) {
    assert(hf_radio_open(false));
    hf_radio_tearoff_timing(true);
    assert(exchange(3, 896, 0) == STATUS_HF_TAG_OK);
    assert(exchange(6, 1216, 0) == STATUS_HF_TAG_OK);
    assert(exchange(10, 1664, 0) == STATUS_HF_TAG_OK);
    assert(exchange(14, 8192, 0) == STATUS_HF_TAG_OK);
    const uint8_t frame[] = {2,0,0x12,0x34};
    assert(last_length == sizeof(frame) && !memcmp(last_frame, frame, sizeof(frame)));
    hf_tearoff_timing_t timing = hf_radio_tearoff_timing_result();
    assert(!slots && last_hardware_tx && acquisitions == 1 && timing.exchanges == 4);
    /* Live frames stop the capture early and skip the complete-buffer decode. */
    assert(live_decodes == 4 && finishes == 4 && last_stop && !decodes);
    assert(timing.dma_cycles == 128000 && !timing.timeslot_cycles && !timing.grant_wait_cycles);
    hf_radio_depth(true); assert(depth);
    hf_radio_depth(false); assert(!depth);
    /* A missed live frame completes the capture and decodes the whole buffer. */
    live_fail = true;
    assert(exchange(6, 1216, 0) == STATUS_HF_TAG_OK && !last_stop && decodes == 1);
    live_fail = false;
    /* A failed exchange keeps hardware TX; there is no timeslot fallback. */
    assert(exchange(6, 1216, 0) == STATUS_HF_TAG_OK && last_hardware_tx);
    assert(hf_radio_close());
}

static void without_usb(void) {
    usb_clock = false;
    assert(hf_radio_open(false) && !acquisitions);
    assert(!st25tb_capture_samples(4, false) && !st25tb_capture_samples(4, true));
    assert(!st25tb_capture_samples(14, false) && !st25tb_capture_samples(14, true));
    assert(exchange(4, 0, 0) == STATUS_PAR_ERR && !slots && !exchanges);
    g_is_ble_connected = true;
    assert(exchange(3, 2048, 15000) == STATUS_HF_TAG_OK);
    assert(exchange(6, 2048, 15000) == STATUS_HF_TAG_OK);
    assert(exchange(10, 3072, 15000) == STATUS_HF_TAG_OK);
    assert(exchange(14, 8192, 25000) == STATUS_HF_TAG_OK);
    assert(slots == 4 && !last_hardware_tx && hf_radio_close());
}

static void type_v(void) {
    assert(hf_radio_open(true) && !acquisitions);
    assert(exchange(0, 11264, 4000) == STATUS_HF_TAG_OK);
    const uint8_t levels[] = {1,0,1};
    assert(last_length == sizeof(levels) && !memcmp(last_frame, levels, sizeof(levels)));
    uint8_t long_frame[23];
    memset(long_frame, 0x42, sizeof(long_frame));
    uint8_t reply[24];
    size_t received;
    expected_slot = 8000;
    assert(hf_radio_exchange(long_frame, 15, 0, reply, sizeof(reply), &received) == 0);
    expected_slot = 10000;
    assert(hf_radio_exchange(long_frame, 22, 0, reply, sizeof(reply), &received) == 0);
    assert(hf_radio_exchange(long_frame, 23, 0, reply, sizeof(reply), &received) == STATUS_PAR_ERR);
    assert(received == 0 && !last_hardware_tx && slots == 3 && hf_radio_close());
    assert(hf_radio_open(false));
    assert(hf_radio_exchange(long_frame, 15, 3, reply, sizeof(reply), &received) == STATUS_PAR_ERR);
    assert(hf_radio_close());
}

static void clock_fallback(void) {
    assert(hf_radio_open(false));
    usb_clock = false;
    assert(exchange(3, 2048, 15000) == STATUS_HF_TAG_OK && !last_hardware_tx);
    usb_clock = true;
    assert(exchange(3, 2048, 15000) == STATUS_HF_TAG_OK && acquisitions == 1);
    assert(hf_radio_close());
}

static void clock_invalidate(void) {
    assert(hf_radio_open(false));
    drop_clock = true;
    assert(exchange(3, 896, 0) == STATUS_HF_ERR_STAT);
    assert(invalidations == 1 && !decodes && !ready && hf_radio_close());
}

static void live_clock_invalidate(void) {
    assert(hf_radio_open(false));
    drop_live_clock = true;
    assert(exchange(3, 896, 0) == STATUS_HF_ERR_STAT);
    assert(live_decodes == 1 && finishes == 1 && !decodes);
    assert(invalidations == 1 && !ready && hf_radio_close());
}

static void deadline(void) {
    assert(hf_radio_open(false));
    hf_radio_tearoff_deadline(1000);
    test_dwt.CYCCNT += 500 * 64000u;
    assert(hf_radio_recover() && recoveries == 1 && acquisitions == 2);
    test_dwt.CYCCNT += 101 * 64000u;
    assert(hf_radio_expired() && !hf_radio_recover() && recoveries == 1);
    assert(exchange(3, 896, 0) == STATUS_BUSY && !exchanges);
    assert(hf_radio_close());
}

static void crc_send(void) {
    usb_clock = false;
    assert(hf_radio_open(false));
    bad_crc = true;
    assert(exchange(6, 2048, 15000) == STATUS_HF_ERR_CRC);
    const uint8_t command[] = {0x0c}, expected[] = {0x0c,0x12,0x34};
    expected_slot = 3000;
    assert(hf_radio_send(command, sizeof(command)) && sends == 1);
    assert(last_length == sizeof(expected) && !memcmp(last_frame, expected, sizeof(expected)));
    assert(!hf_radio_send(NULL, 1) && !hf_radio_send(command, 0));
    assert(hf_radio_close());
}

static void skipped_slot(void) {
    const uint8_t frame[ST25TB_WRITE_FRAME_BYTES] = {0};
    uint32_t measured = 0;
    assert(hf_radio_open(false));
    expected_slot = 25000;
    assert(hf_radio_tearoff_write(frame, 100, &measured));
    assert(measured == 6400 && writes == 1 && cycle_takes == 1);
    skip_slot = true;
    assert(!hf_radio_tearoff_write(frame, 100, &measured));
    assert(!measured && writes == 1 && cycle_takes == 2);
    assert(!hf_radio_tearoff_last_delta_cycles);
    assert(!hf_radio_tearoff_write(frame, 6001, &measured) && slots == 2);
    assert(hf_radio_close());
}

static void tearoff_reads(void) {
    const uint8_t frame[ST25TB_WRITE_FRAME_BYTES] = {0};
    uint32_t measured = 0;
    assert(hf_radio_open(false));
    hf_radio_tearoff_deadline(1000);
    hf_radio_tearoff_timing(true);
    assert(exchange(6, 1216, 0) == STATUS_HF_TAG_OK && last_hardware_tx);
    expected_slot = 25000;
    assert(hf_radio_tearoff_write(frame, 100, &measured));
    assert(measured == 6400 && writes == 1);
    expected_slot = 3000;
    assert(hf_radio_tearoff_reset());
    assert(exchange(6, 1216, 0) == STATUS_HF_TAG_OK && last_hardware_tx);
    expected_slot = 12000;
    assert(hf_radio_st25tb_write(frame));
    assert(writes == 2 && slots == 3 && live_decodes == 2 && !decodes);
    hf_tearoff_timing_t timing = hf_radio_tearoff_timing_result();
    assert(timing.exchanges == 2 && timing.writes == 1 && timing.resets == 1);
    assert(hf_radio_close());
}

static void retained_field(void) {
    assert(hf_radio_open(true) && opens == 1);
    hf_radio_keep();
    test_dwt.CYCCNT += HF_READER_DEADLINE_CYCLES + 1;
    assert(hf_radio_open(true) && opens == 1 && closes == 0);
    assert(exchange(0, 11264, 4000) == STATUS_HF_TAG_OK);
    hf_radio_keep();
    assert(hf_radio_release() && closes == 1);
    assert(hf_radio_release() && closes == 1);
    assert(hf_radio_open(true) && opens == 2);
    hf_radio_keep();
    assert(hf_radio_open(false) && opens == 3 && closes == 2);
    assert(hf_radio_close() && closes == 3);
}

static void vicinity_ble(void) {
    usb_clock = false;
    g_is_ble_connected = true;
    short_slots_only = true;
    assert(hf_radio_open(true));
    /* The normal-priority scheduler refuses long continuous grants. */
    assert(exchange(0, 11264, 0) == STATUS_HF_TAG_OK);
    assert(finishes == 1 && decodes == 1 && slots == 1);
    assert(clock_acquires == 1 && !clock_releases && !last_hardware_tx);
    hf_radio_keep();
    assert(hf_radio_open(true) && clock_acquires == 1);
    assert(exchange(0, 11264, 0) == STATUS_HF_TAG_OK);
    assert(hf_radio_close() && clock_releases == 1 && !clock_owners);
    /* Reader ownership must not release the NFCT startup clock request. */
    clock_owners = HFCLK_NFC;
    assert(hf_radio_open(true));
    assert(clock_owners == (HFCLK_NFC | HFCLK_READER));
    assert(hf_radio_close() && clock_owners == HFCLK_NFC);
}

static void vicinity_clock_failures(void) {
    acquire_error = 1;
    assert(!hf_radio_open(true) && !clock_owners);
    assert(hf_radio_close() && !clock_releases);
    acquire_error = 0; wait_error = 1;
    assert(!hf_radio_open(true) && clock_owners == HFCLK_READER);
    assert(hf_radio_close() && !clock_owners);
    wait_error = 0;
    assert(hf_radio_open(true));
    drop_clock = true;
    assert(exchange(0, 11264, 0) == STATUS_HF_ERR_STAT);
    assert(finishes == 1 && !decodes && invalidations == 1);
    release_error = 1;
    assert(!hf_radio_close() && clock_owners == HFCLK_READER);
    release_error = 0;
    assert(hf_radio_close() && !clock_owners);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "profiles")) profiles();
    else if (!strcmp(argv[1], "direct")) direct();
    else if (!strcmp(argv[1], "without_usb")) without_usb();
    else if (!strcmp(argv[1], "retained")) retained_field();
    else if (!strcmp(argv[1], "vicinity")) type_v();
    else if (!strcmp(argv[1], "vicinity_ble")) vicinity_ble();
    else if (!strcmp(argv[1], "vicinity_clock_failures")) vicinity_clock_failures();
    else if (!strcmp(argv[1], "clock_fallback")) clock_fallback();
    else if (!strcmp(argv[1], "clock_invalidate")) clock_invalidate();
    else if (!strcmp(argv[1], "live_clock_invalidate")) live_clock_invalidate();
    else if (!strcmp(argv[1], "deadline")) deadline();
    else if (!strcmp(argv[1], "crc_send")) crc_send();
    else if (!strcmp(argv[1], "skipped_slot")) skipped_slot();
    else if (!strcmp(argv[1], "tearoff_reads")) tearoff_reads();
    else assert(false);
    puts(argv[1]);
}
