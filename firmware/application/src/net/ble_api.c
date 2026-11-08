#include "ble_api.h"
#include "services/control.h"
#include "classic_job.h"
#include "dataframe.h"
#include "data_cmd.h"
#include "app_status.h"
#include "ble_main.h"
#include <string.h>

static uint32_t streamed_job;

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
    api_operation_t action;
    api_response_t response = {400, "application/octet-stream", 0};

    if (operation == BLE_API_HELLO) {
        if (body_size || lease || argument) goto done;
        /* Protocol version, body limit BE16, reply body limit BE16, flags:
         * bit 0 supports start/state/cancel during a long-running RF job;
         * bit 1 supports querying this connection's negotiated NUS payload;
         * bit 2 supports bounded Classic jobs with unsolicited events;
         * bit 3 supports requesting and inspecting a fast connection profile. */
        const uint8_t hello[] = {BLE_API_VERSION, 4, 0, 2, 0, 15};
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
    if (operation == BLE_API_LINK_PROFILE) {
        if (lease || argument || body_size != 2 || body[0] > 4 || body[1] > 2) goto done;
        ble_link_profile(body[0], body[1], payload);
        response = (api_response_t){200, "application/octet-stream", BLE_LINK_INFO_BYTES};
        goto done;
    }
    if (operation != BLE_API_COMMAND && operation != BLE_API_SLOT && argument) goto done;
    switch (operation) {
    case BLE_API_DEVICE: action = API_DEVICE; break;
    case BLE_API_SLOT: action = API_SLOT; break;
    case BLE_API_STATUS: action = API_STATUS; break;
    case BLE_API_READER_SESSION: action = API_READER_SESSION; break;
    case BLE_API_SLOT_SESSION: action = API_SLOT_SESSION; break;
    case BLE_API_KEEPALIVE: action = API_KEEPALIVE; break;
    case BLE_API_END: action = API_END; break;
    case BLE_API_COMMAND: action = API_COMMAND; break;
    case BLE_API_SETTINGS: action = API_SETTINGS; break;
    case BLE_API_SLOT_OPEN: action = API_SLOT_OPEN; break;
    case BLE_API_SLOT_READ: action = API_SLOT_READ; break;
    case BLE_API_SLOT_BEGIN: action = API_SLOT_BEGIN; break;
    case BLE_API_SLOT_WRITE: action = API_SLOT_WRITE; break;
    case BLE_API_SLOT_COMMIT: action = API_SLOT_COMMIT; break;
    case BLE_API_SLOT_ACTIVATE: action = API_SLOT_ACTIVATE; break;
    case BLE_API_SLOT_NICK: action = API_SLOT_NICK; break;
    case BLE_API_SLOT_ENABLE: action = API_SLOT_ENABLE; break;
    case BLE_API_SLOT_CLEAR: action = API_SLOT_CLEAR; break;
    case BLE_API_TEAROFF_START: action = API_TEAROFF_START; break;
    case BLE_API_TEAROFF_STATE: action = API_TEAROFF_STATE; break;
    case BLE_API_TEAROFF_CANCEL: action = API_TEAROFF_CANCEL; break;
    case BLE_API_UPDATE_INFO: action = API_UPDATE_INFO; break;
    case BLE_API_CLASSIC_JOB_START: action = API_CLASSIC_JOB_START; break;
    case BLE_API_CLASSIC_JOB_CANCEL: action = API_CLASSIC_JOB_CANCEL; break;
    case BLE_API_CLASSIC_JOB_READ: action = API_CLASSIC_JOB_READ; break;
    default: goto done;
    }
    if (operation == BLE_API_SLOT && argument > 7) goto done;
    if (action >= API_KEEPALIVE && action <= API_COMMAND && !lease) goto done;
    if (action >= API_SLOT_OPEN && action <= API_CLASSIC_JOB_READ && !lease) goto done;
    if ((action <= API_UNKNOWN_READ || action == API_READER_SESSION ||
        action == API_SLOT_SESSION || action == API_SETTINGS) && lease) goto done;
    if (action <= API_UNKNOWN_READ && body_size) goto done;
    response = api_execute(API_SOURCE_BLE, action, lease, argument, body, body_size, payload, available);
    if (action == API_CLASSIC_JOB_START && response.status == 200 && response.size == 4)
        streamed_job = read32(payload);
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

void ble_api_poll(void) {
    if (!streamed_job) return;
    uint8_t event[CLASSIC_JOB_EVENT_MAX];
    size_t size = classic_job_read(streamed_job, event, sizeof(event));
    if (!size) return;
    data_frame_tx_t *frame = data_frame_make(DATA_CMD_WEB_EVENT, STATUS_SUCCESS, size, event);
    bool sent = frame && ble_stream_response(frame->buffer, frame->length);
    if (!sent) classic_job_reset();
    if (!sent || (event[7] & 1)) streamed_job = 0;
}
