#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Direct ASK and raw I/Q capture; callers own RF scheduling and frame codecs. */
typedef enum {
    RC522_RAW_NFCB,
    RC522_RAW_NFCV,
} rc522_raw_protocol_t;

/* One prepared, bounded RF window. Type V data contains prepared ASK levels. */
typedef struct {
    const uint8_t *data;
    size_t length;
    uint8_t *samples;
    unsigned sample_count;
    bool hardware_tx;
    /* Exchange returns once the ADC DMA runs; the caller decodes and finishes it. */
    bool live;
    uint32_t *dma_cycles;
} rc522_raw_transfer_t;

/* A failed open still requires close. Recovery keeps the current radio profile. */
bool rc522_raw_open(rc522_raw_protocol_t protocol);
bool rc522_raw_recover(void);
bool rc522_raw_close(void);
/* Consume the previous write delay; skipped windows must report zero. */
uint32_t rc522_raw_take_write_cycles(void);
bool rc522_raw_ready(void);
bool rc522_raw_clock_ready(void);
void rc522_raw_invalidate(void);
bool rc522_raw_acquire_tx(void);
void rc522_raw_depth(bool deeper);

/* Complete hardware sequences; callers own timeslot selection and framing. */
void rc522_raw_exchange(const rc522_raw_transfer_t *transfer);
void rc522_raw_write(const rc522_raw_transfer_t *transfer);
void rc522_raw_tearoff(const rc522_raw_transfer_t *transfer, uint32_t delay_us);
void rc522_raw_reset(void);
void rc522_raw_send(const rc522_raw_transfer_t *transfer);
/* Live capture: samples already written (zeroed beforehand), then stop or complete it. */
unsigned rc522_raw_available(void);
void rc522_raw_finish(bool stop);

/* Passive I/Q stream: repeat the caller's RAM buffer until capture_stop. */
bool rc522_raw_sniff_open(uint8_t gain);
void rc522_raw_capture(uint8_t *samples, unsigned count);
void rc522_raw_capture_stop(void);
