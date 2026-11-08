#pragma once
#include "services/control.h"
api_response_t api_dispatch_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *output, size_t capacity);
