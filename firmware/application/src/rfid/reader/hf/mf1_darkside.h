#ifndef MF1_DARKSIDE_H
#define MF1_DARKSIDE_H
#include <stdint.h>

void mf1_darkside_reset(void);
uint16_t mf1_darkside_step(uint16_t length, const uint8_t *data, uint8_t *output, uint16_t *output_length);
#endif
