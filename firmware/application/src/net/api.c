#include "api.h"
#include "update_api.h"
#include "device_api.h"
#include "slot_api.h"
#include "desfire_reader.h"
#include "lf_commands.h"
#include "app_timer.h"
#include "app_status.h"
#include "data_cmd.h"
#include "rfid_main.h"
#include "nfca_commands.h"
#include "hf_reader.h"
#include "hf_radio.h"
#include "hf_sniff.h"
#include "web_tearoff.h"
#include "usb_main.h"
#include "mf1_darkside.h"
#include "nfca_session.h"
#include <stdio.h>
#include <string.h>
#include "ble_main.h"

#define ST25TB_TEAROFF_ROUTE "/api/st25tb/tearoff/"

static uint32_t session, next_session, renewed;
static device_mode_t previous_mode;
static bool reader_session;
static bool end_after_tearoff;
extern volatile bool m_is_field_on;

void web_session_end(void) {
    if (!session) return;
    if (web_tearoff_active()) {
        web_tearoff_cancel();
        end_after_tearoff = true;
        return;
    }
    nfca_session_close();
    mf1_darkside_reset();
    if (reader_session && previous_mode != DEVICE_MODE_READER) tag_mode_enter();
    web_slots_reset();
    session = 0;
    end_after_tearoff = false;
}

void web_session_poll(void) {
    if (end_after_tearoff && !web_tearoff_active()) web_session_end();
    if (session && app_timer_cnt_diff_compute(app_timer_cnt_get(), renewed) >= APP_TIMER_TICKS(30000)) {
        web_session_end();
    }
}

bool web_session_active(void) {
    web_session_poll();
    if (web_update_entering()) return true;
    return session != 0;
}

static api_response_t result(unsigned status, size_t size) {
    return (api_response_t){status, "application/octet-stream", size};
}

static bool number(const char **text, uint32_t *value) {
    const char *p = *text;
    if (*p < '0' || *p > '9') return false;
    uint32_t n = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        unsigned digit = *p - '0';
        if (n > (UINT32_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *text = p;
    *value = n;
    return true;
}

api_response_t api_dispatch_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *output, size_t capacity) {
    if (web_update_entering() && strcmp(method, "GET")) return result(409, 0);
    /* Existing GET routes inspect metadata only; session and setting changes
     * are POST and remain unavailable until the explicit full restart. */
    if (ble_slave_rf_only() && strcmp(method, "GET"))
        return result(409, 0);
    web_session_poll();
    if (!strcmp(method, "POST") && (usb_cdc_tearoff_running() || hf_sniff_active())) return result(409, 0);
    api_response_t device = web_device_request(method, uri, body, size, output, capacity);
    if (device.status != 404) return device;
    if (!strcmp(method, "GET") && !strcmp(uri, "/api/status")) {
        int n = snprintf((char *)output, capacity,
            "{\"device\":\"Chameleon Ultra\",\"git\":\"%s\",\"busy\":%s}",
            GIT_VERSION, session || usb_cdc_tearoff_running() || hf_sniff_active() ? "true" : "false");
        if (n < 0 || (size_t)n >= capacity) return result(500, 0);
        return (api_response_t){200, "application/json", (size_t)n};
    }
    if (strcmp(method, "POST")) return result(404, 0);
    bool slot_session = !strcmp(uri, "/api/slots/session");
    if (slot_session || !strcmp(uri, "/api/session")) {
        if (size) return result(400, 0);
        if (capacity < 32) return result(500, 0);
        if (session || m_is_field_on || g_is_tag_emulating || nfca_session_active()) return result(409, 0);
        if (next_session == UINT32_MAX) return result(503, 0);
        previous_mode = get_device_mode();
        reader_session = !slot_session;
        session = ++next_session;
        renewed = app_timer_cnt_get();
        web_slots_reset();
        if (reader_session && previous_mode != DEVICE_MODE_READER) reader_mode_enter();
        int n = snprintf((char *)output, capacity, "{\"session\":%lu}", (unsigned long)session);
        return (api_response_t){200, "application/json", (size_t)n};
    }
    uint32_t id, command;
    const char *p;
    if (!strncmp(uri, "/api/end?id=", 12)) {
        p = uri + 12;
        if (!number(&p, &id) || *p) return result(404, 0);
        if (size || id == 0 || id != session) return result(409, 0);
        web_session_end();
        return result(200, 0);
    }
    if (!strncmp(uri, "/api/keepalive?id=", 18)) {
        p = uri + 18;
        if (!number(&p, &id) || *p) return result(404, 0);
        if (size || id == 0 || id != session) return result(409, 0);
        renewed = app_timer_cnt_get();
        return result(200, 0);
    }
    if (!strncmp(uri, ST25TB_TEAROFF_ROUTE, sizeof(ST25TB_TEAROFF_ROUTE) - 1)) {
        const char *operation = uri + sizeof(ST25TB_TEAROFF_ROUTE) - 1;
        const char *query = strstr(operation, "?id=");
        if (!query || query - operation < 1 || query - operation > 6) return result(404, 0);
        p = query + 4;
        if (!number(&p, &id) || *p) return result(404, 0);
        if (!id || id != session || !reader_session) return result(409, 0);
        renewed = app_timer_cnt_get();
        if (query - operation == 5 && !strncmp(operation, "start", 5)) {
            if (m_is_field_on || nfca_session_active() || web_tearoff_active())
                return result(409, 0);
            return result(web_tearoff_start(body, size) ? 200 : 400, 0);
        }
        if (size) return result(400, 0);
        if (query - operation == 6 && !strncmp(operation, "cancel", 6)) {
            web_tearoff_cancel();
            return result(200, 0);
        }
        if (query - operation == 5 && !strncmp(operation, "state", 5)) {
            if (capacity < WEB_TEAROFF_STATE_BYTES) return result(500, 0);
            return result(200, web_tearoff_state(output));
        }
        return result(404, 0);
    }
    if (!strncmp(uri, "/api/slots/", 11)) {
        const char *query = strstr(uri + 11, "?id=");
        if (!query || query - (uri + 11) < 1 || query - (uri + 11) > 11) return result(404, 0);
        char operation[12];
        size_t length = query - (uri + 11);
        memcpy(operation, uri + 11, length); operation[length] = 0;
        p = query + 4;
        if (!number(&p, &id) || *p) return result(404, 0);
        if (id == 0 || id != session || reader_session) return result(409, 0);
        if (size > 1024) return result(413, 0);
        if (m_is_field_on || g_is_tag_emulating) return result(409, 0);
        renewed = app_timer_cnt_get();
        api_response_t response = web_slots_request(operation, body, size, output, capacity);
        renewed = app_timer_cnt_get();
        return response;
    }
    if (strncmp(uri, "/api/command/", 13)) return result(404, 0);
    p = uri + 13;
    if (!number(&p, &command) || strncmp(p, "?id=", 4)) return result(404, 0);
    p += 4;
    if (!number(&p, &id) || *p) return result(404, 0);
    if (id == 0 || id != session || !reader_session) return result(409, 0);
    if (web_tearoff_active()) return result(409, 0);
    if (size > 1024 || capacity < NFCA_COMMAND_OUTPUT_BYTES + 2) return result(413, 0);
    switch (command) {
    case DATA_CMD_DESFIRE_READER:
    case DATA_CMD_HF15_INVENTORY:
    case DATA_CMD_HF15_GET_SYSTEM_INFO:
    case DATA_CMD_HF15_READ_BLOCK:
    case DATA_CMD_HF14B_SCAN:
    case DATA_CMD_ST25TB_INFO:
    case DATA_CMD_ST25TB_READ_BLOCK:
    case DATA_CMD_ST25TB_DUMP:
    case DATA_CMD_ST25TB_WRITE_BLOCK:
    case DATA_CMD_HF15_DUMP:
    case DATA_CMD_SLIX_PRIVACY_SCAN:
    case DATA_CMD_HF15_SECURITY:
    case DATA_CMD_MF1_DARKSIDE_STEP:
    case DATA_CMD_TYPE2_READ:
    case DATA_CMD_HF14A_SCAN:
    case DATA_CMD_MF1_DETECT_PRNG:
    case DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK:
    case DATA_CMD_MF1_READ_ONE_BLOCK:
    case DATA_CMD_NFCA_WRITE_USER_UNIT:
    case DATA_CMD_TYPE2_AUTH_READ:
    case DATA_CMD_MF1_MAGIC_PROBE:
    case DATA_CMD_MF1_UID_SET:
    case DATA_CMD_MF1_TRAILER_SET:
    case DATA_CMD_MF1_INSPECT:
    case DATA_CMD_MF1_MAINTENANCE:
    case DATA_CMD_MF1_BLOCK0_SET:
    case DATA_CMD_MF1_READ_SECTOR:
    case DATA_CMD_MF1_COLLECT_OBSERVATIONS:
    case DATA_CMD_MF1_CAPTURE_SESSION:
    case DATA_CMD_EM410X_SCAN:
    case DATA_CMD_LF_T55XX:
    case DATA_CMD_HIDPROX_SCAN:
    case DATA_CMD_IOPROX_SCAN:
    case DATA_CMD_PAC_SCAN:
    case DATA_CMD_VIKING_SCAN:
    case DATA_CMD_JABLOTRON_SCAN: break;
    default: return result(404, 0);
    }
    if (command != DATA_CMD_MF1_DARKSIDE_STEP && command != DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK &&
        command != DATA_CMD_MF1_CAPTURE_SESSION) mf1_darkside_reset();
    renewed = app_timer_cnt_get();
    uint16_t status, length = 0;
    if (command == DATA_CMD_DESFIRE_READER) {
        status = desfire_reader_command(size, body, output + 2, &length);
    } else if (hf_reader_supports(command)) {
        status = hf_reader_command(command, size, body, output + 2, capacity - 2, &length);
    } else if (command >= DATA_CMD_EM410X_SCAN) {
        nfca_session_close();
        status = hf_radio_release()
            ? web_lf_command(command, size, body, output + 2, &length)
            : STATUS_HF_ERR_STAT;
    } else if (command == DATA_CMD_MF1_CAPTURE_SESSION) {
        if (size != 1 || body[0] > 2) return result(400, 0);
        if (body[0] == 0) nfca_session_close();
        if (body[0] == 1) nfca_session_open();
        if (body[0] == 2) { output[2] = nfca_session_active(); length = 1; }
        status = STATUS_SUCCESS;
    } else {
        if (command != DATA_CMD_MF1_COLLECT_OBSERVATIONS && command != DATA_CMD_TYPE2_READ &&
            command != DATA_CMD_MF1_READ_SECTOR &&
            command != DATA_CMD_MF1_DARKSIDE_STEP && command != DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK) nfca_session_close();
        status = nfca_prepare(command, size, body);
        if (status == STATUS_SUCCESS) {
            status = nfca_command(command, size, (uint8_t *)body, output + 2, &length);
            nfca_finish(command);
        }
    }
    renewed = app_timer_cnt_get();
    output[0] = status >> 8; output[1] = status;
    return result(200, length + 2);
}
