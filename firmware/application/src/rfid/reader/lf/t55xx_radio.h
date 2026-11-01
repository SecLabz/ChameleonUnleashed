#pragma once
#include "protocols/t55xx_decode.h"

bool t55xx_radio_detect(t55xx_demod_t *demod);
bool t55xx_radio_read(uint8_t block,
    const t55xx_demod_t *demod, uint32_t *word);
void t55xx_radio_write(uint8_t block, uint32_t word);
