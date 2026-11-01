#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { unsigned status; const char *type; size_t size; } api_response_t;
api_response_t api_dispatch_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *output, size_t capacity);
void web_session_poll(void);
bool web_session_active(void);

void web_session_end(void);
