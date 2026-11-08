#pragma once
#include "control.h"

void web_slots_reset(void);
uint16_t slot_snapshot_request(const uint8_t *body, size_t size, uint8_t *output,
    size_t capacity, uint16_t *length);
api_response_t web_slots_request(api_operation_t operation, const uint8_t *body,
    size_t size, uint8_t *output, size_t capacity);
