#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "hf_sniff.h"
#include "iso15693/frame.h"
#include "nfc_14a_frame_codec.h"
#include "parity.h"
#include "crc_utils.h"

typedef enum { DEVICE_MODE_READER, DEVICE_MODE_TAG } device_mode_t;
static device_mode_t mode = DEVICE_MODE_READER;
volatile bool m_is_field_on;
static bool passive;
static bool sensing;
static bool timer_owned;
static bool fail_timer;
static unsigned timer_owners;
static unsigned critical_depth;
static unsigned restored;
static unsigned ticks;
static unsigned micros;
static uint8_t registers[64];
static uint8_t fifo[64];
static bool pending_tag;
static bool interleave_reader;
static bool spi_burst_active;
static void (*callback)(const uint8_t *, uint16_t);
static struct {
    uint32_t INTENSET;
} nfct;
#define NRF_NFCT (&nfct)
#define NFCT_INTENSET_TXFRAMESTART_Msk 1
#define HF_ANT_SEL 1
#define CommandReg 1
#define FIFOLevelReg 10
#define ComIrqReg 4
#define ErrorReg 6
#define Control522Reg 12
#define FIFODataReg 9
#define RxModeReg 19
#define MfRxReg 29
#define RFCfgReg 38
#define BitFramingReg 13
#define TxControlReg 20
#define PCD_IDLE 0
#define PCD_RECEIVE 8
#define STATUS_HF_ERR_STAT 2
static void nrf_gpio_pin_clear(unsigned pin) {
    (void)pin;
}

static void nrf_gpio_pin_set(unsigned pin) {
    (void)pin;
}

static void pcd_14a_reader_reset(void) {
    restored++;
}

static void pcd_14a_reader_antenna_off(void) {
    registers[TxControlReg] &= ~3u;
}

static void write_register_single(uint8_t address, uint8_t value) {
    assert(!spi_burst_active);
    registers[address] = value;
    if (address == ComIrqReg) {
        pending_tag = false;
    }
}

static uint8_t read_register_single(uint8_t address) {
    if (address == ComIrqReg) {
        return pending_tag ? 0x20 : 0;
    }
    return registers[address];
}

static void read_register_buffer(uint8_t address, uint8_t *out, uint8_t length) {
    assert(address == FIFODataReg && length > 0 && length <= 64);
    spi_burst_active = true;
    memcpy(out, fifo, length);
    registers[FIFOLevelReg] = 0;
    if (interleave_reader) {
        assert(!critical_depth);
        interleave_reader = false;
        uint8_t wake = 0x52;
        callback(&wake, 7);
    }
    spi_burst_active = false;
}

static void nfc_tag_14a_clear_tx_sniff_cb(void) {
}

static void nfc_tag_14a_set_tx_sniff_cb(void (*cb)(const uint8_t *, uint16_t)) {
    (void)cb;
}
typedef unsigned nrfx_timer_t;
typedef unsigned nrf_timer_event_t;
typedef struct {
    unsigned frequency;
    unsigned bit_width;
} nrfx_timer_config_t;
#define NRFX_TIMER_INSTANCE(n) (n)
#define NRFX_TIMER_DEFAULT_CONFIG { 0, 0 }
#define NRF_TIMER_FREQ_1MHz 1
#define NRF_TIMER_BIT_WIDTH_32 32
#define NRF_TIMER_CC_CHANNEL0 0
#define NRFX_SUCCESS 0
#define STATUS_SUCCESS 0x68
#define STATUS_PAR_ERR 0x60
#define STATUS_BUSY 0x76
#define APP_TIMER_TICKS(n) (n)
#define CRITICAL_REGION_ENTER() do { critical_depth++;
#define CRITICAL_REGION_EXIT() critical_depth--; } while (0)
#define __DMB() ((void)0)
static uint32_t app_timer_cnt_get(void) {
    return ticks;
}

static uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t before) {
    return now - before;
}

static void nfc_tag_14a_clear_sniff_cb(void) {
    callback = NULL;
}

static void nfc_tag_14a_set_sniff_cb(void (*cb)(const uint8_t *, uint16_t)) {
    callback = cb;
}

static void nfc_tag_14a_set_sniff_passive(bool value) {
    passive = value;
}

static void nfc_tag_14a_sense_switch(bool value) {
    sensing = value;
    if (value) {
        assert(passive);
    }
}

static device_mode_t get_device_mode(void) {
    return mode;
}

static void tag_mode_enter(void) {
    assert(!passive && !sensing && !timer_owned);
    mode = DEVICE_MODE_TAG;
}

static bool release_ok = true;
static bool hf_radio_release(void) { return release_ok; }

static void reader_mode_enter(void) {
    assert(!passive && !sensing && !timer_owned);
    mode = DEVICE_MODE_READER;
}

static unsigned nrfx_timer_init(const nrfx_timer_t *timer, nrfx_timer_config_t *config,
        void (*handler)(nrf_timer_event_t, void *)) {
    (void)timer;
    (void)handler;
    assert(config->frequency == 1 && config->bit_width == 32);
    if (fail_timer) {
        return 1;
    }
    timer_owners++;
    timer_owned = true;
    return 0;
}

static void nrfx_timer_uninit(const nrfx_timer_t *timer) {
    (void)timer;
    assert(timer_owners);
    timer_owners--;
    timer_owned = timer_owners != 0;
}

static void nrfx_timer_enable(const nrfx_timer_t *timer) {
    if (*timer == 3) {
        micros = 0;
    }
}

static uint32_t nrfx_timer_capture(const nrfx_timer_t *timer, unsigned channel) {
    (void)timer;
    (void)channel;
    assert(timer_owned);
    return micros++;
}

static bool nfc_tag_14a_checks_crc(uint8_t *data, size_t length) {
    uint8_t crc[2];
    calc_14a_crc_lut(data, length - 2, crc);
    return !memcmp(crc, data + length - 2, 2);
}

static bool vicinity_available;

static bool nfcv_sniff_start(uint8_t gain) {
    (void)gain;
    return vicinity_available;
}
static void nfcv_sniff_stop(void) {}
static void nfcv_sniff_poll(void) {}

/* PRODUCTION_CAPTURE */

static uint8_t output[HF_SNIFF_BATCH_BYTES];
static uint16_t size;
static uint16_t command_for(bool type_v, uint8_t operation) {
    uint8_t request[] = {operation, 6};
    return hf_sniff_command(type_v, operation == 0 ? 2 : 1, request, output, &size);
}

static uint16_t command(uint8_t operation) {
    return command_for(false, operation);
}

static void receive_bytes(const uint8_t *bytes, size_t length) {
    uint8_t raw[300] = {0};
    uint8_t parity[257];
    for (size_t i = 0; i < length; i++) {
        parity[i] = oddparity8(bytes[i]);
    }
    nfc_14a_pack_bytes(bytes, parity, length, raw);
    assert(callback && passive && sensing);
    callback(raw, length * 9);
}

static void inject_tag(const uint8_t *data, unsigned length, bool encrypted_parity) {
    uint8_t parity[64];
    assert(length <= 56 && registers[MfRxReg] == 0x10);
    for (unsigned i = 0; i < length; i++) {
        parity[i] = oddparity8(data[i]) ^ encrypted_parity;
    }
    memset(fifo, 0, sizeof(fifo));
    nfc_14a_pack_bytes(data, parity, length, fifo);
    unsigned raw_bits = length * 9;
    registers[FIFOLevelReg] = (raw_bits + 7) / 8;
    registers[Control522Reg] = raw_bits % 8;
    registers[ErrorReg] = 0;
    pending_tag = true;
}

static void reader_request(void) {
    uint8_t wake = 0x52;
    callback(&wake, 7);
    assert(registers[CommandReg] == PCD_RECEIVE);
    assert(command(1) == STATUS_SUCCESS && output[2] == 1 && output[39] == 0);
}

int main(void) {
    m_is_field_on = true;
    assert(command(0) == STATUS_BUSY && !active && !timer_owned);
    m_is_field_on = false;
    release_ok = false;
    assert(command(0) == STATUS_HF_ERR_STAT && !active && !timer_owned);
    release_ok = true;
    assert(command(0) == STATUS_SUCCESS && active && sensing && passive);
    assert(command(0) == STATUS_BUSY);
    assert(command(7) == STATUS_PAR_ERR);
    uint8_t reqa = 0x26;
    micros = 999;
    callback(&reqa, 7);
    uint8_t select[] = {0x93, 0x20};
    micros = 2000;
    receive_bytes(select, sizeof(select));
    assert(command(1) == STATUS_SUCCESS && output[2] == 2);
    assert(output[28] == (999 & 255) && output[36] == 7 && output[42] == 0x26);
    assert(output[51] == 16 && output[53] == (HF_SNIFF_DECODED | HF_SNIFF_PARITY_CHECKED | HF_SNIFF_PARITY_VALID));
    assert(output[57] == 0x93 && output[58] == 0x20);
    for (unsigned i = 0; i < 50; i++) {
        callback(&reqa, 7);
    }
    assert(dropped == 10 && seen == 52);
    assert(command(2) == STATUS_SUCCESS && !active && !passive && !sensing && mode == DEVICE_MODE_READER);
    unsigned drained = output[2];
    while (drained < 40) {
        assert(command(1) == STATUS_SUCCESS);
        drained += output[2];
    }
    assert(drained == 40 && command(1) == STATUS_SUCCESS && output[2] == 0);
    assert(restored == 2 && !timer_owned);
    assert(command(0) == STATUS_SUCCESS);
    uint8_t long_frame[80] = {0};
    receive_bytes(long_frame, sizeof(long_frame));
    assert(command(1) == STATUS_SUCCESS && output[38] & HF_SNIFF_TRUNCATED && output[41] == 64 && invalid == 1);
    micros = UINT32_MAX - 1000;
    hf_sniff_poll();
    micros = 20;
    callback(&reqa, 7);
    assert(queued_frame(written - 1)->end_us == (uint64_t)UINT32_MAX + 21);
    ticks += 2000;
    hf_sniff_poll();
    assert(!active && !passive && !timer_owned && restored == 4);
    assert(command(0) == STATUS_SUCCESS);
    assert(registers[RxModeReg] == 0x08 && registers[MfRxReg] == 0x10);
    const uint8_t atqa[] = {4, 0};
    inject_tag(atqa, sizeof(atqa), false);
    tag_receive(false);
    assert(tag_seen == 0); // Idle reception never interprets reader modulation as a tag.
    reader_request();
    assert(!pending_tag); // Reader EOF discarded the stale FIFO before arming.
    inject_tag(atqa, sizeof(atqa), false);
    tag_receive(false);
    assert(!awaiting_tag && registers[CommandReg] == PCD_IDLE);
    assert(command(1) == STATUS_SUCCESS && output[2] == 1);
    assert(output[39] == 1 && output[42] == 4 && output[43] == 0);
    assert(output[38] == (HF_SNIFF_DECODED | HF_SNIFF_PARITY_CHECKED | HF_SNIFF_PARITY_VALID));

    reader_request();
    inject_tag(atqa, sizeof(atqa), false);
    reader_request();
    assert(overruns == 1 && output[20] == 1 && tag_seen == 1);
    inject_tag(atqa, sizeof(atqa), false);
    interleave_reader = true;
    tag_receive(false);
    assert(awaiting_tag); // Deferred EOF arming resumes after the entire SPI transaction.
    assert(command(1) == STATUS_SUCCESS && output[2] == 2);
    assert(output[39] == 0 && output[42] == 0x52);
    assert(output[54] == 1 && output[57] == 4);
    uint8_t ciphertext[18];
    memset(ciphertext, 0x91, sizeof(ciphertext));
    inject_tag(ciphertext, sizeof(ciphertext), true);
    tag_receive(false);
    assert(!awaiting_tag);
    assert(command(1) == STATUS_SUCCESS && output[2] == 1);
    assert(output[36] == 144 && output[41] == 18);
    assert((output[38] & (HF_SNIFF_DECODED | HF_SNIFF_PARITY_CHECKED | HF_SNIFF_PARITY_VALID)) == (HF_SNIFF_DECODED | HF_SNIFF_PARITY_CHECKED));
    assert(!memcmp(output + 42, ciphertext, sizeof(ciphertext)));

    reader_request();
    fifo[0] = 0x1f;
    registers[FIFOLevelReg] = 1;
    registers[Control522Reg] = 5;
    pending_tag = true;
    tag_receive(false);
    assert(awaiting_tag);
    inject_tag(atqa, sizeof(atqa), false);
    tag_receive(false);
    assert(command(1) == STATUS_SUCCESS && output[2] == 2);
    assert(!(output[38] & HF_SNIFF_DECODED) && output[53] & HF_SNIFF_DECODED);

    reader_request();
    registers[FIFOLevelReg] = 0;
    registers[ErrorReg] = 0x10;
    pending_tag = true;
    tag_receive(false);
    assert(overruns == 2);
    const uint8_t sak[] = {0x08, 0xb6, 0xdd};
    inject_tag(sak, sizeof(sak), false);
    pending_tag = false;
    hf_sniff_transmit_observed(NULL, 0);
    assert(command(2) == STATUS_SUCCESS && output[24] == 1);
    assert(output[2] == 1 && output[38] & HF_SNIFF_TRUNCATED && output[39] == 1);
    assert(!(output[38] & (HF_SNIFF_DECODED | HF_SNIFF_CRC_CHECKED | HF_SNIFF_CRC_VALID)));
    assert(output[41] == sizeof(sak) && !memcmp(output + 42, sak, sizeof(sak)));
    mode = DEVICE_MODE_TAG;
    fail_timer = true;
    assert(command(0) == STATUS_BUSY && !active && !passive && !timer_owned && mode == DEVICE_MODE_TAG);

    fail_timer = false;
    assert(command(0) == STATUS_SUCCESS);
    callback(&reqa, 7);
    hf_sniff_stop();
    assert(command_for(true, 0) == STATUS_BUSY);
    assert(command(1) == STATUS_SUCCESS && output[2] == 1 && output[42] == reqa);
    assert(command(0) == STATUS_SUCCESS);
    callback(&reqa, 7);
    uint8_t *legacy = hf_sniff_legacy_acquire();
    assert(!active && !passive && !timer_owned && mode == DEVICE_MODE_TAG);
    memset(legacy, 0xa5, HF_SNIFF_LEGACY_BYTES);
    assert(command(1) == STATUS_SUCCESS && output[2] == 0 && output[12] == 1);
    assert(hf_sniff_legacy_acquire() == legacy);
    assert(command(1) == STATUS_SUCCESS && output[2] == 0 && output[12] == 1);

    vicinity_available = true;
    assert(command_for(true, 0) == STATUS_SUCCESS);
    uint8_t response[143];
    memset(response, 0x5a, sizeof(response));
    hf_sniff_publish(1, response, sizeof(response), HF_SNIFF_DECODED | HF_SNIFF_CRC_CHECKED | HF_SNIFF_CRC_VALID);
    assert(command(1) == STATUS_BUSY && active);
    assert(command_for(true, 1) == STATUS_SUCCESS && output[2] == 1 && output[41] == 143);
    assert(!memcmp(output + 42, response, sizeof(response)));
    assert(output[36] == (1144 & 255) && output[37] == (1144 >> 8));
    for (unsigned i = 0; i < 23; i++) {
        hf_sniff_publish(1, response, sizeof(response), HF_SNIFF_DECODED | HF_SNIFF_CRC_CHECKED | HF_SNIFF_CRC_VALID);
    }
    assert(command_for(true, 2) == STATUS_SUCCESS && output[2] == 16 && output[12] == 1);
    assert(command_for(true, 1) == STATUS_SUCCESS && output[2] == 6);
    assert(command_for(true, 1) == STATUS_SUCCESS && output[2] == 0);
}
