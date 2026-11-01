#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "st25tb_reader.h"

/* NFC-B/V exchanges own framing, scheduling and deadlines.
 * Only successful explicit raw keep requests can retain the frontend. */
/* NFC-B uses direct TX/live capture when the USB clock and TX resources allow it. */
bool hf_radio_open(bool vicinity);
bool hf_radio_close(void);
void hf_radio_keep(void);
/* Close only a frontend retained by raw; harmless for the NFC-A driver. */
bool hf_radio_release(void);
void hf_radio_depth(bool deeper);
bool hf_radio_expired(void);
/* Reply capacity is at least 24 bytes; NFC-B expected includes CRC bytes. */
uint16_t hf_radio_exchange(const uint8_t *data, size_t length,
    size_t expected, uint8_t *reply, size_t capacity, size_t *received);
/* ST25TB writes have no RF acknowledgement; keep the carrier on for EEPROM commit. */
bool hf_radio_st25tb_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES]);
/* Private ST25TB operation: transmit the selected-tag write and reset in one slot. */
bool hf_radio_tearoff_write(const uint8_t frame[ST25TB_WRITE_FRAME_BYTES], uint32_t delay_us,
    uint32_t *request_to_write_cycles);
bool hf_radio_tearoff_reset(void);
/* Restore the reader after a failed SPI/DMA transfer without ending tearoff. */
bool hf_radio_recover(void);
extern volatile uint32_t hf_radio_tearoff_last_off_cycles;
void hf_radio_tearoff_deadline(uint32_t milliseconds);
typedef struct {
    uint32_t timeslot_cycles;
    uint32_t grant_wait_cycles;
    uint32_t dma_cycles;
    uint32_t decode_cycles;
    uint32_t exchanges;
    uint32_t writes;
    uint32_t resets;
} hf_tearoff_timing_t;
void hf_radio_tearoff_timing(bool enabled);
hf_tearoff_timing_t hf_radio_tearoff_timing_result(void);

/* Send a Type B command without a reply, preserving its settling interval. */
bool hf_radio_send(const uint8_t *data, size_t length);
