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
    BLE_API_HELLO = 0,
    BLE_API_DEVICE = 1,
    BLE_API_SLOT = 2,
    BLE_API_STATUS = 3,
    BLE_API_READER_SESSION = 4,
    BLE_API_SLOT_SESSION = 5,
    BLE_API_KEEPALIVE = 6,
    BLE_API_END = 7,
    BLE_API_COMMAND = 8,
    BLE_API_SETTINGS = 9,
    BLE_API_SLOT_OPEN = 10,
    BLE_API_SLOT_READ = 11,
    BLE_API_SLOT_BEGIN = 12,
    BLE_API_SLOT_WRITE = 13,
    BLE_API_SLOT_COMMIT = 14,
    BLE_API_SLOT_ACTIVATE = 15,
    BLE_API_SLOT_NICK = 16,
    BLE_API_SLOT_ENABLE = 17,
    BLE_API_SLOT_CLEAR = 18,
    BLE_API_TEAROFF_START = 19,
    BLE_API_TEAROFF_STATE = 20,
    BLE_API_TEAROFF_CANCEL = 21,
    BLE_API_UPDATE_INFO = 22,
    BLE_API_TRANSPORT_INFO = 23,
    BLE_API_LINK_PROFILE = 24,
    BLE_API_CLASSIC_JOB_START = 25,
    BLE_API_CLASSIC_JOB_CANCEL = 26,
    BLE_API_CLASSIC_JOB_READ = 27,
};

uint16_t ble_api_request(const uint8_t *request, size_t size,
    uint8_t *output, size_t capacity, uint16_t *length);

void ble_api_poll(void);
