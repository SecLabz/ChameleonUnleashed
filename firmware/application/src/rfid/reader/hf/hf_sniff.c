#include "hf_sniff.h"
#include "hf_radio.h"
#include "nfcv_sniff.h"
#include "iso15693/frame.h"
#include "nfc_14a.h"
#include "nfc_14a_frame_codec.h"
#include "rfid_main.h"
#include "app_status.h"
#include "app_timer.h"
#include "app_util_platform.h"
#include "nrfx_timer.h"
#include "parity.h"
#include "rc522.h"
#include <string.h>

#define QUEUE_FRAMES 40u
#define VICINITY_QUEUE_FRAMES 22u
#define RECORD_ALIGNMENT_BYTES 8u
#define FRAME_BYTES 64u
#define HEADER_BYTES 28u
#define RECORD_BYTES 14u
#define LEASE_MS 2000u
#define RX_IGNORE_SHORT_NOISE 0x08u
#define RX_KEEP_PARITY_BITS 0x10u
#define POLL_BUDGET_US 500u
#define REPLY_GUARD_US 800u
#define USB_SERVICE_LIMIT_US 5000u

extern volatile bool m_is_field_on;

static const nrfx_timer_t clock_timer = NRFX_TIMER_INSTANCE(3);
typedef struct {
    uint64_t end_us;
    uint16_t bits;
    uint8_t flags;
    uint8_t direction;
    uint8_t errors;
    uint8_t bytes;
    uint8_t data[];
} capture_frame_t;

static union {
    uint64_t alignment;
    uint8_t bytes[HF_SNIFF_LEGACY_BYTES];
} storage;
static bool vicinity;

static unsigned queue_capacity(void) {
    return vicinity ? VICINITY_QUEUE_FRAMES : QUEUE_FRAMES;
}

static capture_frame_t *queued_frame(unsigned index) {
    unsigned bytes = vicinity ? ISO15693_RESPONSE_MAX_BYTES : FRAME_BYTES;
    unsigned stride = (offsetof(capture_frame_t, data) + bytes + RECORD_ALIGNMENT_BYTES - 1u) &
        ~(RECORD_ALIGNMENT_BYTES - 1u);
    return (capture_frame_t *)(storage.bytes + (index % queue_capacity()) * stride);
}

static volatile uint32_t written;
static volatile uint32_t consumed;
static volatile uint32_t seen;
static volatile uint32_t tag_seen;
static volatile uint32_t overruns;
static volatile uint32_t tx_events;
static volatile uint32_t dropped;
static volatile uint32_t invalid;
static volatile bool active;
static uint32_t renewed;
static uint32_t last_us;
static volatile uint32_t last_reader_us;
static volatile uint32_t armed_reader;
static volatile bool awaiting_tag;
static volatile bool tag_bus_owned;
static uint64_t elapsed_us;
static device_mode_t previous_mode;

static void tag_receive(bool stopping);
static void tag_arm(void);

uint8_t *hf_sniff_legacy_acquire(void) {
    hf_sniff_stop();
    dropped += written - consumed;
    consumed = written;
    return storage.bytes;
}

static uint64_t timestamp_us(void) {
    /* Main-loop callers mask application IRQs; the producer is the NFCT ISR.
     * Poll at least once per lease to extend the 32-bit timer across wraps. */
    uint32_t now = nrfx_timer_capture(&clock_timer, NRF_TIMER_CC_CHANNEL0);
    elapsed_us += (uint32_t)(now - last_us);
    last_us = now;
    return elapsed_us;
}

static uint8_t decode_raw_frame(const uint8_t *raw, uint16_t raw_bits,
        uint8_t data[FRAME_BYTES], uint16_t *size_bits) {
    unsigned bits = raw_bits - raw_bits / 9u;
    unsigned bytes = (bits + 7u) / 8u;
    uint8_t flags = HF_SNIFF_DECODED;
    if (bytes > FRAME_BYTES) {
        bytes = FRAME_BYTES;
        bits = FRAME_BYTES * 8u;
        flags = HF_SNIFF_TRUNCATED;
    }
    memset(data, 0, FRAME_BYTES);
    /* Preserve the input: NFCT DMA bytes also feed the emulation dispatcher.
     * Both receivers retain ninth bits, including encrypted Classic parity. */
    unsigned full_bytes = raw_bits / 9u;
    if (full_bytes > FRAME_BYTES) {
        full_bytes = FRAME_BYTES;
    }
    nfc_14a_unpack_bytes(raw, full_bytes, data, NULL);
    for (unsigned bit = full_bytes * 8u; bit < bits; bit++) {
        unsigned source = bit + bit / 8u;
        data[bit / 8u] |= ((raw[source / 8u] >> (source % 8u)) & 1u) << (bit % 8u);
    }
    if (raw_bits >= 9u) {
        flags |= HF_SNIFF_PARITY_CHECKED | HF_SNIFF_PARITY_VALID;
        for (unsigned i = 0; i < full_bytes; i++) {
            unsigned parity_bit = i * 9u + 8u;
            if (((raw[parity_bit / 8u] >> (parity_bit % 8u)) & 1u) != oddparity8(data[i])) {
                flags &= ~HF_SNIFF_PARITY_VALID;
            }
        }
    }
    if (bits >= 24u && bits % 8u == 0 && !(flags & HF_SNIFF_TRUNCATED)) {
        flags |= HF_SNIFF_CRC_CHECKED;
        if (nfc_tag_14a_checks_crc(data, bytes)) {
            flags |= HF_SNIFF_CRC_VALID;
        }
    }
    *size_bits = bits;
    return flags;
}

static void receive(const uint8_t *raw, uint16_t raw_bits) {
    if (!active) {
        return;
    }
    uint64_t end_us = timestamp_us();
    last_reader_us = last_us;
    seen++;
    /* Arm before decoding/copying the reader frame. Waiting for the main USB
     * pass can exceed the tag's 86 us minimum response delay. The ISR may use
     * SPI only while the main-thread receiver has explicitly released it. */
    if (!tag_bus_owned) {
        tag_arm();
    }
    if (written - consumed == queue_capacity()) {
        dropped++;
        return;
    }
    capture_frame_t *record = queued_frame(written);
    uint16_t bits;
    uint8_t flags = decode_raw_frame(raw, raw_bits, record->data, &bits);
    unsigned bytes = (bits + 7u) / 8u;
    if ((flags & HF_SNIFF_TRUNCATED) || ((flags & HF_SNIFF_PARITY_CHECKED) && !(flags & HF_SNIFF_PARITY_VALID))) {
        invalid++;
    }
    record->direction = 0;
    record->errors = 0;
    record->end_us = end_us;
    record->bits = bits;
    record->bytes = bytes;
    record->flags = flags;
    __DMB();
    written++;
}

bool hf_sniff_active(void) {
    return active;
}

void hf_sniff_stop(void) {
    if (!active) {
        return;
    }
    if (vicinity) {
        nfcv_sniff_stop();
    } else {
        tag_receive(true);
    }
    CRITICAL_REGION_ENTER();
    active = false;
    nfc_tag_14a_clear_sniff_cb();
    nfc_tag_14a_clear_tx_sniff_cb();
    nrfx_timer_uninit(&clock_timer);
    CRITICAL_REGION_EXIT();
    /* Disable sensing before restoring normal replies: no transient response
     * is allowed to collide with the physical card during teardown. */
    if (!vicinity) {
        nfc_tag_14a_sense_switch(false);
    }
    nfc_tag_14a_set_sniff_passive(false);
    write_register_single(CommandReg, PCD_IDLE);
    pcd_14a_reader_reset();
    nrf_gpio_pin_clear(HF_ANT_SEL);
    if (previous_mode != DEVICE_MODE_READER) {
        tag_mode_enter();
    }
}

void hf_sniff_poll(void) {
    if (!active) {
        return;
    }
    if (app_timer_cnt_diff_compute(app_timer_cnt_get(), renewed) >= APP_TIMER_TICKS(LEASE_MS)) {
        hf_sniff_stop();
        return;
    }
    if (vicinity) {
        nfcv_sniff_poll();
        timestamp_us();
        return;
    }
    /* Main-thread SPI ownership lets NFCT defer an EOF rearm without colliding
     * with a FIFO transaction. Tight polling normally yields to USB after
     * 500 us; NFCT reception remains enabled throughout SPI transactions. */
    uint32_t started = nrfx_timer_capture(&clock_timer, NRF_TIMER_CC_CHANNEL0);
    uint32_t now;
    do {
        tag_receive(false);
        now = nrfx_timer_capture(&clock_timer, NRF_TIMER_CC_CHANNEL0);
        /* Selection replies finish within this guard after the reader EOF.
         * Defer USB work across that interval, but bound every service window
         * even under continuous traffic so cancellation and draining stay live. */
    } while ((uint32_t)(now - started) < POLL_BUDGET_US ||
        ((uint32_t)(now - started) < USB_SERVICE_LIMIT_US &&
         (uint32_t)(now - last_reader_us) < REPLY_GUARD_US));
    CRITICAL_REGION_ENTER();
    timestamp_us();
    CRITICAL_REGION_EXIT();
}

static void tag_receive_start(void) {
    write_register_single(CommandReg, PCD_IDLE);
    write_register_single(FIFOLevelReg, 0x80);
    write_register_single(ComIrqReg, 0x7f);
    write_register_single(CommandReg, PCD_RECEIVE);
}

static void tag_arm(void) {
    /* An unread completion is about to be flushed. Report the loss even when
     * the hardware FIFO itself did not overflow during a USB service pass. */
    if (awaiting_tag && (read_register_single(ComIrqReg) & 0x22)) {
        overruns++;
    }
    armed_reader = seen;
    tag_receive_start();
    awaiting_tag = true;
}

static void tag_receive_owned(bool stopping) {
    /* ISO14443-A is half duplex. Reader modulation can look like Manchester
     * noise to RC522. Flush it at the observed reader EOF, then listen for one
     * tag reply; do not keep decoding the following reader command. */
    if (!stopping && armed_reader != seen) {
        tag_arm();
    }
    if (!awaiting_tag) {
        return;
    }
    uint8_t irq = read_register_single(ComIrqReg);
    bool partial = !(irq & 0x22);
    if (partial && (!stopping || !(read_register_single(FIFOLevelReg) & 0x7f))) {
        return;
    }
    write_register_single(CommandReg, PCD_IDLE);
    uint8_t errors = read_register_single(ErrorReg);
    uint8_t bytes = read_register_single(FIFOLevelReg) & 0x7f;
    uint8_t last_bits = read_register_single(Control522Reg) & 7;
    if (errors & 0x10) {
        overruns++;
    }
    if (bytes && bytes <= FRAME_BYTES) {
        uint8_t raw[FRAME_BYTES];
        uint8_t data[FRAME_BYTES];
        unsigned raw_bits = last_bits ? (bytes - 1u) * 8u + last_bits : bytes * 8u;
        read_register_buffer(FIFODataReg, raw, bytes);
        uint16_t bits;
        uint8_t flags = decode_raw_frame(raw, raw_bits, data, &bits);
        bytes = (bits + 7u) / 8u;
        if (partial || (errors & 0x10)) {
            flags |= HF_SNIFF_TRUNCATED;
            flags &= ~(HF_SNIFF_DECODED | HF_SNIFF_CRC_CHECKED | HF_SNIFF_CRC_VALID);
        }
        if ((errors & 0x19) || (raw_bits != 4u && raw_bits % 9u != 0)) {
            flags &= ~HF_SNIFF_DECODED;
        }
        /* Only queue publication masks NFCT. The SPI burst and CRC work above
         * must not hold off reader reception or its next RX rearm. */
        CRITICAL_REGION_ENTER();
        tag_seen++;
        if (!(flags & HF_SNIFF_DECODED) || (errors & 0x1f)) {
            invalid++;
        }
        if (written - consumed == queue_capacity()) {
            dropped++;
        } else {
            capture_frame_t *record = queued_frame(written);
            record->end_us = timestamp_us();
            record->bits = bits;
            record->bytes = bytes;
            record->direction = 1;
            record->errors = errors;
            record->flags = flags;
            memcpy(record->data, data, bytes);
            __DMB();
            written++;
        }
        CRITICAL_REGION_EXIT();
        if ((flags & HF_SNIFF_DECODED) && !errors) {
            awaiting_tag = false;
        }
    }
    if (awaiting_tag) {
        tag_receive_start();
    }
}

static void tag_receive(bool stopping) {
    tag_bus_owned = true;
    __DMB();
    tag_receive_owned(stopping);
    /* An EOF that interrupted a FIFO burst deferred arming to this owner.
     * Mask only the short rearm/release handoff: otherwise a last-moment EOF
     * could stay deferred across the next USB pass and miss the tag preamble. */
    CRITICAL_REGION_ENTER();
    if (!stopping && armed_reader != seen) {
        tag_arm();
    }
    __DMB();
    tag_bus_owned = false;
    CRITICAL_REGION_EXIT();
}

void hf_sniff_transmit_observed(const uint8_t *data, uint16_t bits) {
    (void)data;
    (void)bits;
    tx_events++;
}

static void timer_handler(nrf_timer_event_t event, void *context) {
    (void)event;
    (void)context;
}

static void put_le(uint8_t *out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) {
        out[i] = value >> (8u * i);
    }
}

uint16_t hf_sniff_command(bool type_v, uint16_t length, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    *size = 0;
    if (!length || data[0] > 2 || (data[0] == 0 ? (length != 2 || data[1] > 7) : length != 1)) {
        return STATUS_PAR_ERR;
    }
    if (type_v != vicinity && (active || data[0] != 0)) {
        return STATUS_BUSY;
    }
    hf_sniff_poll();
    if (data[0] == 0) {
        if (active || m_is_field_on) {
            return STATUS_BUSY;
        }
        if (!hf_radio_release()) {
            return STATUS_HF_ERR_STAT;
        }
        previous_mode = get_device_mode();
        reader_mode_enter();
        pcd_14a_reader_reset();
        write_register_single(RxModeReg, RX_IGNORE_SHORT_NOISE);
        /* Keep the ninth bits: encrypted Classic parity must not terminate or
         * fragment reception. Both receiver paths share software deparity. */
        write_register_single(MfRxReg, RX_KEEP_PARITY_BITS);
        write_register_single(RFCfgReg, data[1] << 4);
        write_register_single(BitFramingReg, 0);
        if (read_register_single(TxControlReg) & 3u) {
            pcd_14a_reader_antenna_off();
            if (previous_mode != DEVICE_MODE_READER) {
                tag_mode_enter();
            }
            return STATUS_HF_ERR_STAT;
        }
        nrfx_timer_config_t config = NRFX_TIMER_DEFAULT_CONFIG;
        config.frequency = NRF_TIMER_FREQ_1MHz;
        config.bit_width = NRF_TIMER_BIT_WIDTH_32;
        if (nrfx_timer_init(&clock_timer, &config, timer_handler) != NRFX_SUCCESS) {
            if (previous_mode != DEVICE_MODE_READER) {
                tag_mode_enter();
            }
            return STATUS_BUSY;
        }
        nrfx_timer_enable(&clock_timer);
        if (type_v && !nfcv_sniff_start(data[1])) {
            nrfx_timer_uninit(&clock_timer);
            if (previous_mode != DEVICE_MODE_READER) {
                tag_mode_enter();
            }
            return STATUS_BUSY;
        }
        vicinity = type_v;
        written = 0;
        consumed = 0;
        seen = 0;
        armed_reader = 0;
        awaiting_tag = false;
        tag_bus_owned = false;
        tag_seen = 0;
        overruns = 0;
        tx_events = 0;
        dropped = 0;
        invalid = 0;
        last_us = 0;
        last_reader_us = 0;
        elapsed_us = 0;
        renewed = app_timer_cnt_get();
        if (vicinity) {
            active = true;
        } else {
            nfc_tag_14a_set_sniff_passive(true);
            nfc_tag_14a_set_sniff_cb(receive);
            nfc_tag_14a_set_tx_sniff_cb(hf_sniff_transmit_observed);
            active = true;
            nrf_gpio_pin_set(HF_ANT_SEL);
            nfc_tag_14a_sense_switch(true);
            NRF_NFCT->INTENSET = NFCT_INTENSET_TXFRAMESTART_Msk;
        }
        return STATUS_SUCCESS;
    }
    if (data[0] == 2) {
        hf_sniff_stop();
    } else if (active) {
        renewed = app_timer_cnt_get();
    }
    memset(output, 0, HEADER_BYTES);
    output[0] = 2;
    output[1] = active;
    unsigned count = 0;
    unsigned offset = HEADER_BYTES;
    /* NFCT ISR and briefly masked main-thread publication, one consumer. Keep RF interrupts live while
     * copying a USB batch. A slot is released only after copying its payload. */
    uint32_t available = written;
    __DMB();
    while (consumed != available && count < 16u) {
        capture_frame_t *record = queued_frame(consumed);
        put_le(output + offset, record->end_us, 8);
        put_le(output + offset + 8, record->bits, 2);
        output[offset + 10] = record->flags;
        output[offset + 11] = record->direction;
        output[offset + 12] = record->errors;
        output[offset + 13] = record->bytes;
        memcpy(output + offset + RECORD_BYTES, record->data, record->bytes);
        offset += RECORD_BYTES + record->bytes;
        __DMB();
        consumed++;
        count++;
    }
    put_le(output + 2, count, 2);
    put_le(output + 4, seen, 4);
    put_le(output + 8, tag_seen, 4);
    put_le(output + 12, dropped, 4);
    put_le(output + 16, invalid, 4);
    put_le(output + 20, overruns, 4);
    put_le(output + 24, tx_events, 4);
    *size = offset;
    return STATUS_SUCCESS;
}

void hf_sniff_overrun(void) {
    overruns++;
}

void hf_sniff_publish(uint8_t direction, const uint8_t *data, unsigned bytes,
        uint8_t flags) {
    if (direction) {
        tag_seen++;
    } else {
        seen++;
    }
    if (!(flags & HF_SNIFF_DECODED) || !(flags & HF_SNIFF_CRC_VALID)) {
        invalid++;
    }
    if (written - consumed == queue_capacity()) {
        dropped++;
        return;
    }
    capture_frame_t *record = queued_frame(written);
    record->end_us = timestamp_us();
    record->bits = bytes * 8u;
    record->bytes = bytes;
    record->flags = flags;
    record->direction = direction;
    record->errors = 0;
    memcpy(record->data, data, bytes);
    __DMB();
    written++;
}
