#include "device_api.h"
#include "app_status.h"
#include "ble_main.h"
#include "fds_util.h"
#include "rfid_main.h"
#include "settings.h"
#include "tag_persistence.h"
#include <stdio.h>
#include <string.h>

static api_response_t response(unsigned status, size_t size) {
    return (api_response_t){status, "application/octet-stream", size};
}

static api_response_t device_info(uint8_t *output, size_t capacity) {
    int n = snprintf((char *)output, capacity,
        "{\"version\":\"%s\",\"git\":\"%s\",\"chip\":\"%08lx%08lx\","
        "\"battery_mv\":%u,\"battery_percent\":%u,\"reader\":%s,\"active_slot\":%u,"
        "\"busy\":%s,\"settings\":{\"animation\":%u,\"press_a\":%u,\"press_b\":%u,"
        "\"long_a\":%u,\"long_b\":%u,\"sleep\":%lu,\"long_ms\":%u,\"pairing\":%s}}",
        APP_FW_SEMVER, GIT_VERSION,
        (unsigned long)NRF_FICR->DEVICEID[1], (unsigned long)NRF_FICR->DEVICEID[0],
        batt_lvl_in_milli_volts, percentage_batt_lvl,
        get_device_mode() == DEVICE_MODE_READER ? "true" : "false",
        tag_emulation_get_slot(), web_session_active() ? "true" : "false",
        settings_get_animation_config(), settings_get_button_press_config('A'),
        settings_get_button_press_config('B'), settings_get_long_button_press_config('A'),
        settings_get_long_button_press_config('B'),
        (unsigned long)(settings_get_sleep_timeout() / 1000),
        settings_get_long_press_threshold(), settings_get_ble_pairing_enable() ? "true" : "false");
    if (n < 0 || (size_t)n >= capacity) return response(500, 0);
    return (api_response_t){200, "application/json", (size_t)n};
}

static size_t nickname(uint8_t slot, tag_sense_type_t sense, uint8_t *output) {
    fds_slot_record_map_t map;
    get_fds_map_by_slot_sense_type_for_nick(slot, sense, &map);
    uint8_t bytes[36] = {0};
    uint16_t size = sizeof(bytes);
    if (!fds_read_sync(map.id, map.key, &size, bytes) || size < 1 ||
        bytes[0] > 32 || bytes[0] + 1 > size) {
        output[0] = 0;
        return 1;
    }
    memcpy(output, bytes, bytes[0] + 1);
    return bytes[0] + 1;
}

static api_response_t slot_info(uint8_t slot, uint8_t *output, size_t capacity) {
    if (capacity < 79) return response(500, 0);
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(slot, &types);
    output[0] = types.tag_hf >> 8; output[1] = types.tag_hf;
    output[2] = types.tag_lf >> 8; output[3] = types.tag_lf;
    output[4] = is_slot_enabled(slot, TAG_SENSE_HF) |
                (is_slot_enabled(slot, TAG_SENSE_LF) << 1);
    size_t length = 5;
    length += nickname(slot, TAG_SENSE_HF, output + length);
    length += nickname(slot, TAG_SENSE_LF, output + length);
    for (unsigned sense = TAG_SENSE_HF; sense >= TAG_SENSE_LF; sense--) {
        uint32_t revision = tag_emulation_place_revision(slot, sense);
        for (unsigned i = 0; i < 4; i++) output[length++] = revision >> (24 - 8 * i);
    }
    return response(200, length);
}

/* One setting per request; the success response follows persistent save. */
static api_response_t set_setting(const uint8_t *body, size_t size,
                                  uint8_t *output, size_t capacity) {
    extern volatile bool m_is_field_on;
    if (web_session_active() || m_is_field_on) return response(409, 0);
    if (capacity < 2) return response(500, 0);
    if (size < 2) return response(400, 0);
    unsigned key = body[0], value = body[1];
    switch (key) {
    case 0:
        if (size != 2 || value >= SettingsAnimationModeMAX) return response(400, 0);
        settings_set_animation_config(value);
        break;
    case 1: case 2: case 3: case 4:
        if (size != 2 || value > SettingsButtonNfcFieldGenerator) return response(400, 0);
        if (key < 3) settings_set_button_press_config(key == 1 ? 'A' : 'B', value);
        else settings_set_long_button_press_config(key == 3 ? 'A' : 'B', value);
        break;
    case 5:
        if (size != 2 || value < SETTINGS_SLEEP_TIMEOUT_MIN_S ||
            value > SETTINGS_SLEEP_TIMEOUT_MAX_S) return response(400, 0);
        settings_set_sleep_timeout(value);
        break;
    case 6:
        if (size != 3) return response(400, 0);
        value = (body[1] << 8) | body[2];
        if (value < SETTINGS_LONG_PRESS_THRESHOLD_MIN_MS) return response(400, 0);
        settings_set_long_press_threshold(value);
        break;
    case 7:
        if (size != 2 || value > 1) return response(400, 0);
        settings_set_ble_pairing_enable(value);
        break;
    case 8:
        if (size != BLE_PAIRING_KEY_LEN + 1) return response(400, 0);
        for (size_t i = 1; i < size; i++)
            if (body[i] < '0' || body[i] > '9') return response(400, 0);
        {
            uint8_t pin[BLE_PAIRING_KEY_LEN];
            memcpy(pin, body + 1, sizeof(pin));
            settings_set_ble_connect_key(pin);
        }
        break;
    default: return response(400, 0);
    }
    uint8_t status = settings_save_config();
    output[0] = 0; output[1] = status;
    return response(200, 2);
}

api_response_t web_device_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *output, size_t capacity) {
    if (!strcmp(method, "GET")) {
        if (!strcmp(uri, "/api/device")) return device_info(output, capacity);
        if (!strncmp(uri, "/api/slot/", 10) && strlen(uri) == 11 &&
            uri[10] >= '0' && uri[10] <= '7')
            return slot_info(uri[10] - '0', output, capacity);
    }
    if (!strcmp(method, "POST") && !strcmp(uri, "/api/settings"))
        return set_setting(body, size, output, capacity);
    return response(404, 0);
}
