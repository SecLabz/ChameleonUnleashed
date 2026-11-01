#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_TEAROFF_STATE_BYTES 20u

bool web_tearoff_start(const uint8_t *request, size_t size);
bool web_tearoff_active(void);
bool web_tearoff_running(void);
void web_tearoff_cancel(void);
void web_tearoff_poll(void);
void web_tearoff_service(void);
bool web_tearoff_cancelled(void);
void web_tearoff_progress(uint8_t kind, uint32_t value, uint16_t delay_us);
size_t web_tearoff_state(uint8_t output[WEB_TEAROFF_STATE_BYTES]);
