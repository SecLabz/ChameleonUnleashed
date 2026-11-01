#pragma once
#include "api.h"

void web_slots_reset(void);
uint16_t slot_snapshot_request(const uint8_t *body, size_t size, uint8_t *output,
    size_t capacity, uint16_t *length);
api_response_t web_slots_request(const char *operation, const uint8_t *body,
    size_t size, uint8_t *output, size_t capacity);
