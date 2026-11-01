#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iso15693/frame.h"

/* Silent NFCT reader RX and MFRC522 tag RX for NFC-A and NFC-V.
 * Timestamps are software RX-completion observations in us, not RF edges. */
#define HF_SNIFF_BATCH_BYTES (28u + 16u * (14u + ISO15693_RESPONSE_MAX_BYTES))
#define HF_SNIFF_LEGACY_BYTES 3800u

bool hf_sniff_active(void);
void hf_sniff_poll(void);
void hf_sniff_stop(void);
uint16_t hf_sniff_command(bool vicinity, uint16_t length, const uint8_t *data,
    uint8_t *output, uint16_t *size);

/* Stop live capture and discard its queued frames before reusing shared RAM. */
uint8_t *hf_sniff_legacy_acquire(void);

#define HF_SNIFF_DECODED 1u
#define HF_SNIFF_PARITY_CHECKED 2u
#define HF_SNIFF_PARITY_VALID 4u
#define HF_SNIFF_CRC_CHECKED 8u
#define HF_SNIFF_CRC_VALID 16u
#define HF_SNIFF_TRUNCATED 32u

void hf_sniff_transmit_observed(const uint8_t *data, uint16_t bits);
void hf_sniff_overrun(void);
void hf_sniff_publish(uint8_t direction, const uint8_t *data, unsigned bytes,
    uint8_t flags);
