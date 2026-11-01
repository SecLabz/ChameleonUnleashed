#pragma once
#include "frame.h"
#include <stddef.h>

#define NFCV_SNIFF_HIGH_INTEGRALS 256u
#define NFCV_SNIFF_LOW_INTEGRALS 1024u

typedef enum {
    NFCV_SNIFF_READER_NONE,
    NFCV_SNIFF_READER_FRAME,
    NFCV_SNIFF_READER_SLOT,
} nfcv_sniff_reader_event_t;

typedef struct {
    iso15693_frame_t frame;
    uint32_t last_fall;
    uint32_t reference;
    unsigned symbols;
    unsigned inventory_slots;
    bool previous_lock;
    bool acquired;
    bool slow;
    bool truncated;
} nfcv_sniff_reader_t;

typedef struct {
    float *integral[4];
    unsigned integral_count;
    float sum[4];
    float dc_i;
    float dc_q;
    float cosine;
    float sine;
    uint32_t samples;
    unsigned bins;
    unsigned position;
    unsigned half;
    unsigned rate;
    unsigned length;
    bool acquired;
    bool truncated;
    bool eof_checked;
    uint8_t data[ISO15693_RESPONSE_MAX_BYTES];
} nfcv_sniff_tag_t;

/* Completed frames include bad CRC; a slot event retains the Inventory rate. */
nfcv_sniff_reader_event_t nfcv_sniff_reader_feed(nfcv_sniff_reader_t *reader, bool lock, uint32_t cycles);
bool nfcv_sniff_reader_stop(nfcv_sniff_reader_t *reader);
void nfcv_sniff_tag_reset(nfcv_sniff_tag_t *tag, bool high_rate, float *workspace);
/* Feed at most 32 ADC I/Q bytes; NULL advances a measured unsampled gap.
 * True means EOF or a physical decode failure. */
bool nfcv_sniff_tag_feed(nfcv_sniff_tag_t *tag, const uint8_t *samples, unsigned count);

bool nfcv_sniff_tag_gap(nfcv_sniff_tag_t *tag, unsigned count, uint8_t before, uint8_t after);
