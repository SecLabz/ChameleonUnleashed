#pragma once
#include <stdint.h>
/* 0=end; 1=begin (optional UID length/UID); 2=native public command/parameters.
 * Exchange returns native status followed by data, aggregating DESFire AF.
 * The RF lease expires through nfca_session_poll, including abandoned USB. */
uint16_t desfire_reader_command(uint16_t size, const uint8_t *data,
    uint8_t *output, uint16_t *length);
void desfire_reader_invalidate(void);
