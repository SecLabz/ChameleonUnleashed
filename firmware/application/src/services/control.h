#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { unsigned status; const char *type; size_t size; } api_response_t;
typedef enum { API_SOURCE_HTTP, API_SOURCE_BLE } api_source_t;
typedef enum {
    API_DEVICE,
    API_SLOT,
    API_STATUS,
    API_UPDATE_INFO,
    API_UNKNOWN_READ,
    API_READER_SESSION,
    API_SLOT_SESSION,
    API_KEEPALIVE,
    API_END,
    API_COMMAND,
    API_SETTINGS,
    API_SLOT_OPEN,
    API_SLOT_READ,
    API_SLOT_BEGIN,
    API_SLOT_WRITE,
    API_SLOT_COMMIT,
    API_SLOT_ACTIVATE,
    API_SLOT_NICK,
    API_SLOT_ENABLE,
    API_SLOT_CLEAR,
    API_TEAROFF_START,
    API_TEAROFF_STATE,
    API_TEAROFF_CANCEL,
    API_CLASSIC_JOB_START,
    API_CLASSIC_JOB_CANCEL,
    API_CLASSIC_JOB_READ,
    API_UNKNOWN_WRITE,
} api_operation_t;
api_response_t api_execute(api_source_t source, api_operation_t operation,
    uint32_t id, uint16_t argument, const uint8_t *body, size_t size,
    uint8_t *output, size_t capacity);
void web_session_poll(void);
bool web_session_active(void);
void web_session_end(void);
/* Interrupt-safe notification; RF/storage cleanup belongs to the main loop. */
void web_session_disconnect(api_source_t source);
