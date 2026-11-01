#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nfcb_tx.h"

/* Acquire once per bounded reader command; close also unwinds a failed open. */
bool rc522_tx_open(void);
void rc522_tx_close(void);
bool rc522_tx_send(const uint8_t *bytes, size_t length, uint8_t low);

#define RC522_CARRIER_LEVEL 32u
#define RC522_ASK_SHALLOW 16u
#define RC522_ASK_DEEP 8u
#define RC522_TX_WRITE_BYTES 2u

/* SPI write address for CWGsCfgReg (0x28), one write per Type-B ETU. */
#define RC522_TX_ADDRESS 0x50u
static inline size_t rc522_tx_encode(const uint8_t *bytes, size_t length,
        uint8_t low, uint8_t *out, size_t capacity) {
    if (!bytes || !length || length > NFCB_TX_MAX_FRAME_BYTES || !out) {
        return 0;
    }
    size_t symbols = NFCB_TX_FRAMING_SYMBOLS + length * NFCB_TX_SYMBOLS_PER_BYTE;
    if (capacity < RC522_TX_WRITE_BYTES * symbols) {
        return 0;
    }

    for (size_t i = 0; i < symbols; i++) {
        out[2 * i] = RC522_TX_ADDRESS;
        out[2 * i + 1] = RC522_CARRIER_LEVEL;
    }
    for (unsigned i = 0; i < NFCB_TX_SOF_LOW_SYMBOLS; i++) {
        out[2 * i + 1] = low;
    }

    for (size_t b = 0; b < length; b++) {
        size_t pos = NFCB_TX_SOF_SYMBOLS + b * NFCB_TX_SYMBOLS_PER_BYTE;
        out[2 * pos + 1] = low;
        for (unsigned bit = 0; bit < 8; bit++) {
            out[2 * (pos + 1 + bit) + 1] = bytes[b] & (1u << bit) ? RC522_CARRIER_LEVEL : low;
        }
    }

    for (size_t i = symbols - (NFCB_TX_EOF_LOW_SYMBOLS + 1); i < symbols - 1; i++) {
        out[2 * i + 1] = low;
    }
    return symbols;
}
