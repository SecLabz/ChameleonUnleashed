#pragma once
#include "control.h"

api_response_t web_device_request(api_operation_t operation, uint16_t argument,
    const uint8_t *body, size_t size, uint8_t *output, size_t capacity);
