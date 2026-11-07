#pragma once
#include <stddef.h>
#include <stdint.h>

/* Version 1 request: version u8, correlation BE32, operation u8, lease BE32,
 * argument BE16, body. Reply: version u8, correlation BE32, service status
 * BE16, encoding u8 (0 binary, 1 JSON), body. All inside command 1090. */
#define BLE_API_VERSION 1u
#define BLE_API_REQUEST_HEADER 12u
#define BLE_API_RESPONSE_HEADER 8u
#define BLE_API_BODY_MAX 1024u
#define BLE_API_RESPONSE_MAX (BLE_API_RESPONSE_HEADER + 512u)

enum {
    BLE_API_HELLO, BLE_API_DEVICE, BLE_API_SLOT, BLE_API_STATUS,
    BLE_API_READER_SESSION, BLE_API_SLOT_SESSION, BLE_API_KEEPALIVE, BLE_API_END,
    BLE_API_COMMAND, BLE_API_SETTINGS,
    BLE_API_SLOT_OPEN, BLE_API_SLOT_READ, BLE_API_SLOT_BEGIN, BLE_API_SLOT_WRITE,
    BLE_API_SLOT_COMMIT, BLE_API_SLOT_ACTIVATE, BLE_API_SLOT_NICK, BLE_API_SLOT_ENABLE,
    BLE_API_SLOT_CLEAR, BLE_API_TEAROFF_START, BLE_API_TEAROFF_STATE, BLE_API_TEAROFF_CANCEL,
    BLE_API_UPDATE_INFO, BLE_API_TRANSPORT_INFO, BLE_API_LINK_PROFILE,
    BLE_API_CLASSIC_JOB_START, BLE_API_CLASSIC_JOB_CANCEL, BLE_API_CLASSIC_JOB_READ
};

uint16_t ble_api_request(const uint8_t *request, size_t size,
    uint8_t *output, size_t capacity, uint16_t *length);
