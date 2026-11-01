#pragma once
#include <stdint.h>

/* Request: operation, parameter, UID length, UID. Output capacity >=32 bytes.
 * 0=version, 1=four pages, 2=signature, 3=counter, 4=tearing status.
 * 5=FAST_READ: parameter is the first page; append the last page after UID.
 * A range contains 1..8 pages; responses never exceed 32 bytes. */
uint16_t type2_read_command(uint16_t size, const uint8_t *data,
    uint8_t *output, uint16_t *length);
