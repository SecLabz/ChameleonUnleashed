#pragma once
#include "api.h"

api_response_t web_device_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *output, size_t capacity);
