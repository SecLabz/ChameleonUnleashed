#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "model.h"

void st25tb_receiver_reset(void);
bool st25tb_receiver_idle(void);
/* After a reply, skip the ADC recovery from load modulation before seeding the baseline. */
void st25tb_receiver_settle(void);
/* Set the next pair time in 2 us units, including gaps between DMA banks. */
void st25tb_receiver_time(unsigned now);
bool st25tb_receiver_pair(uint8_t first, uint8_t second,
                          uint8_t frame[ST25TB_REQUEST_MAX], unsigned *length);

/* Consume complete sample pairs, returning bytes consumed; zero length means no frame. */
unsigned st25tb_receiver_batch(const uint8_t *samples, unsigned size,
                               uint8_t frame[ST25TB_REQUEST_MAX], unsigned *length);
