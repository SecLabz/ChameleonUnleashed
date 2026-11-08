#pragma once
#include "services/control.h"
api_response_t web_update_info(uint8_t *out, size_t capacity);
api_response_t web_update_application_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *out, size_t capacity);
void web_update_poll(void);
bool web_update_entering(void);
