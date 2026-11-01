#ifndef MF1_CAPTURE_STATS_H
#define MF1_CAPTURE_STATS_H

#include <stdbool.h>
#include <stdint.h>

#define MF1_CAPTURE_STATS_BYTES 40

typedef struct {
    bool enabled;
    uint32_t setup_cycles;
    uint32_t select_cycles;
    uint32_t auth_cycles;
    uint32_t target_cycles;
    uint32_t housekeeping_cycles;
    uint32_t wait_cycles;
    uint32_t timeouts;
    uint32_t wakeups;
    uint32_t observations;
} mf1_capture_stats_t;

extern mf1_capture_stats_t mf1_capture_stats;
void mf1_capture_stats_start(bool enabled);
uint32_t mf1_capture_clock(void);
uint32_t mf1_capture_elapsed(uint32_t started);
void mf1_capture_stats_encode(uint8_t output[MF1_CAPTURE_STATS_BYTES]);

#endif
