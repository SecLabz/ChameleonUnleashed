#ifndef RF_TIMESLOT_H
#define RF_TIMESLOT_H
#include <stdbool.h>
#include <stdint.h>

/* Runs bounded radio work inside a SoftDevice radio callback. No SDK calls
 * or decoding outside that RF deadline belong in the callback. */
/* With extension enabled, start with a short grant and negotiate up to the
 * requested duration before calling work. The crystal is guaranteed. */
bool rf_timeslot_run(uint32_t microseconds, bool high_priority, bool extend, void (*work)(void));
uint32_t rf_timeslot_last_run_cycles(void);
uint32_t rf_timeslot_last_grant_wait_cycles(void);
/* Absolute 64 MHz deadline; valid only inside the work callback. */
uint32_t rf_timeslot_deadline(void);
uint32_t rf_timeslot_granted_us(void);
#endif
