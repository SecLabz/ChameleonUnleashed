#pragma once
#include <stdint.h>

/* HTTP LF commands; output capacity is at least LF_T55XX_PLAN_BYTES. */
uint16_t web_lf_command(uint16_t command, uint16_t size, const uint8_t *data,
    uint8_t *output, uint16_t *length);
