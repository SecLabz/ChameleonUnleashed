#include "ble_api.h"
#include "api.h"
#include "app_status.h"
#include "ble_main.h"
#include <stdio.h>
#include <string.h>

static uint32_t read32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
        (uint32_t)bytes[2] << 8 | bytes[3];
}

uint16_t ble_api_request(const uint8_t *request, size_t size,
    uint8_t *output, size_t capacity, uint16_t *length) {
    *length = 0;
    if (!request || size < BLE_API_REQUEST_HEADER ||
        size > BLE_API_REQUEST_HEADER + BLE_API_BODY_MAX ||
        request[0] != BLE_API_VERSION || !read32(request + 1) ||
        capacity < BLE_API_RESPONSE_MAX) return STATUS_PAR_ERR;
    unsigned operation = request[5];
    uint32_t lease = read32(request + 6);
    unsigned argument = (unsigned)request[10] << 8 | request[11];
    size_t body_size = size - BLE_API_REQUEST_HEADER;
    const uint8_t *body = request + BLE_API_REQUEST_HEADER;
    uint8_t *payload = output + BLE_API_RESPONSE_HEADER;
    size_t available = capacity - BLE_API_RESPONSE_HEADER;
    const char *method = "POST", *path = NULL;
    char uri[64];
    api_response_t response = {400, "application/octet-stream", 0};

    if (operation == BLE_API_HELLO) {
        if (body_size || lease || argument) goto done;
        /* Protocol version, body limit BE16, reply body limit BE16, flags:
         * bit 0 supports start/state/cancel during a long-running RF job;
         * bit 1 supports querying this connection's negotiated NUS payload. */
        const uint8_t hello[] = {BLE_API_VERSION, 4, 0, 2, 0, 3};
        memcpy(payload, hello, sizeof(hello));
        response = (api_response_t){200, "application/octet-stream", sizeof(hello)};
        goto done;
    }
    if (operation == BLE_API_TRANSPORT_INFO) {
        if (body_size || lease || argument) goto done;
        uint16_t limit = ble_nus_payload_limit();
        payload[0] = 1;
        payload[1] = limit >> 8;
        payload[2] = limit;
        response = (api_response_t){200, "application/octet-stream", 3};
        goto done;
    }
    if (operation != BLE_API_COMMAND && operation != BLE_API_SLOT && argument) goto done;
    switch (operation) {
    case BLE_API_DEVICE: path = "/api/device"; method = "GET"; break;
    case BLE_API_SLOT:
        if (argument > 7) goto done;
        snprintf(uri, sizeof(uri), "/api/slot/%u", argument);
        path = uri; method = "GET"; break;
    case BLE_API_STATUS: path = "/api/status"; method = "GET"; break;
    case BLE_API_UPDATE_INFO: path = "/api/update"; method = "GET"; break;
    case BLE_API_READER_SESSION: path = "/api/session"; break;
    case BLE_API_SLOT_SESSION: path = "/api/slots/session"; break;
    case BLE_API_SETTINGS: path = "/api/settings"; break;
    case BLE_API_KEEPALIVE: path = "/api/keepalive"; break;
    case BLE_API_END: path = "/api/end"; break;
    case BLE_API_COMMAND:
        if (!lease) goto done;
        snprintf(uri, sizeof(uri), "/api/command/%u?id=%lu", argument, (unsigned long)lease);
        break;
    default:
        if (operation >= BLE_API_SLOT_OPEN && operation <= BLE_API_SLOT_CLEAR) {
            static const char *const names[] = {"open", "read", "begin", "write", "commit",
                "activate", "nick", "enable", "clear"};
            snprintf(uri, sizeof(uri), "/api/slots/%s?id=%lu",
                names[operation - BLE_API_SLOT_OPEN], (unsigned long)lease);
        } else if (operation >= BLE_API_TEAROFF_START && operation <= BLE_API_TEAROFF_CANCEL) {
            static const char *const names[] = {"start", "state", "cancel"};
            snprintf(uri, sizeof(uri), "/api/st25tb/tearoff/%s?id=%lu",
                names[operation - BLE_API_TEAROFF_START], (unsigned long)lease);
        } else goto done;
    }
    if (operation == BLE_API_KEEPALIVE || operation == BLE_API_END) {
        snprintf(uri, sizeof(uri), "%s?id=%lu", path, (unsigned long)lease);
        path = uri;
    } else if (!path) path = uri;
    if (operation >= BLE_API_KEEPALIVE && operation <= BLE_API_COMMAND && !lease) goto done;
    if ((operation >= BLE_API_SLOT_OPEN && operation <= BLE_API_TEAROFF_CANCEL) && !lease) goto done;
    if ((operation <= BLE_API_SLOT_SESSION || operation == BLE_API_SETTINGS ||
        operation == BLE_API_UPDATE_INFO) && lease) goto done;
    if (!strcmp(method, "GET") && body_size) goto done;
    response = api_dispatch_from(API_SOURCE_BLE, method, path, body, body_size, payload, available);
done:
    if (response.size > available) return STATUS_PAR_ERR;
    output[0] = BLE_API_VERSION;
    memcpy(output + 1, request + 1, 4);
    output[5] = response.status >> 8;
    output[6] = response.status;
    output[7] = strstr(response.type, "application/json") != NULL;
    *length = BLE_API_RESPONSE_HEADER + response.size;
    return STATUS_SUCCESS;
}
