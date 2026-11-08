#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "device_api.h"
#include "rfid_main.h"
#include "settings.h"
#include "app_status.h"

test_ficr_t test_ficr = {{0x12345678, 0xabcdef01}};
uint16_t batt_lvl_in_milli_volts = 4012;
uint8_t percentage_batt_lvl = 87;
volatile bool m_is_field_on;
static bool busy, pairing;
static unsigned mutations, saves;
static uint8_t save_status = STATUS_SUCCESS, animation, short_press[2], long_press[2], sleep_s = 8;
static uint16_t long_ms = 1000;
static uint8_t pin[6];
static uint8_t nick_size = 4;
bool web_session_active(void) { return busy; }
int get_device_mode(void) { return 2; }
uint8_t tag_emulation_get_slot(void) { return 3; }
uint32_t tag_emulation_place_revision(uint8_t slot,tag_sense_type_t sense) {return 0x12340000u+slot*16+sense;}
void tag_emulation_get_specific_types_by_slot(uint8_t slot, tag_slot_specific_type_t *types) {
    *types = (tag_slot_specific_type_t){1001, 100};
}
bool is_slot_enabled(uint8_t slot, tag_sense_type_t sense) { return sense == TAG_SENSE_HF; }
void get_fds_map_by_slot_sense_type_for_nick(uint8_t slot, tag_sense_type_t sense, void *map) { memset(map, 0, 4); }
bool fds_read_sync(uint16_t id, uint16_t key, uint16_t *length, uint8_t *buffer) {
    memset(buffer, 'x', *length); buffer[0] = nick_size; return true;
}
uint8_t settings_get_animation_config(void) { return animation; }
void settings_set_animation_config(uint8_t value) { mutations++; animation = value; }
uint8_t settings_get_button_press_config(char which) { return short_press[which == 'B']; }
void settings_set_button_press_config(char which, uint8_t value) { mutations++; short_press[which == 'B'] = value; }
uint8_t settings_get_long_button_press_config(char which) { return long_press[which == 'B']; }
void settings_set_long_button_press_config(char which, uint8_t value) { mutations++; long_press[which == 'B'] = value; }
uint32_t settings_get_sleep_timeout(void) { return sleep_s * 1000; }
void settings_set_sleep_timeout(uint8_t value) { mutations++; sleep_s = value; }
uint16_t settings_get_long_press_threshold(void) { return long_ms; }
void settings_set_long_press_threshold(uint16_t value) { mutations++; long_ms = value; }
bool settings_get_ble_pairing_enable(void) { return pairing; }
void settings_set_ble_pairing_enable(bool value) { mutations++; pairing = value; }
void settings_set_ble_connect_key(uint8_t *value) { mutations++; memcpy(pin, value, 6); }
uint8_t settings_save_config(void) { saves++; return save_status; }

int main(void) {
    uint8_t output[512] = {0};
    api_response_t r = web_device_request(API_DEVICE, 0, NULL, 0, output, sizeof(output));
    assert(r.status == 200 && !strcmp(r.type, "application/json"));
    assert(strstr((char *)output, "\"version\":\"1.0.0\""));
    assert(strstr((char *)output, "\"chip\":\"abcdef0112345678\""));
    assert(strstr((char *)output, "\"active_slot\":3"));
    assert(web_device_request(API_DEVICE, 0, NULL, 0, output, 10).status == 500);
    r = web_device_request(API_SLOT, 7, NULL, 0, output, sizeof(output));
    assert(r.status == 200 && r.size == 23);
    assert(output[0] == 3 && output[1] == 233 && output[2] == 0 && output[3] == 100 && output[4] == 1);
    assert(output[5] == 4 && output[10] == 4);
    nick_size = 255;
    r = web_device_request(API_SLOT, 0, NULL, 0, output, sizeof(output));
    assert(r.status == 200 && r.size == 15 && output[5] == 0 && output[6] == 0);
    assert(web_device_request(API_SLOT, 8, NULL, 0, output, sizeof(output)).status == 404);
    uint8_t data[8] = {0, 3};
    busy = true;
    assert(web_device_request(API_SETTINGS, 0, data, 2, output, sizeof(output)).status == 409);
    busy = false; m_is_field_on = true;
    assert(web_device_request(API_SETTINGS, 0, data, 2, output, sizeof(output)).status == 409);
    m_is_field_on = false;
    assert(mutations == 0 && saves == 0);
    r = web_device_request(API_SETTINGS, 0, data, 2, output, sizeof(output));
    assert(r.status == 200 && r.size == 2 && output[1] == STATUS_SUCCESS && animation == 3 && saves == 1);
    unsigned before = mutations;
    const uint8_t invalid[][3] = {{0, 4}, {1, 6}, {5, 4}, {5, 61}, {6, 0, 199}, {7, 2}, {9, 0}};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(web_device_request(API_SETTINGS, 0, invalid[i], invalid[i][0] == 6 ? 3 : 2, output, sizeof(output)).status == 400);
    }
    assert(web_device_request(API_SETTINGS, 0, NULL, 0, output, sizeof(output)).status == 400);
    assert(mutations == before);
    data[0] = 6; data[1] = 0xff; data[2] = 0xff;
    assert(web_device_request(API_SETTINGS, 0, data, 3, output, sizeof(output)).status == 200 && long_ms == 65535);
    data[0] = 8; memcpy(data + 1, "012345", 6);
    assert(web_device_request(API_SETTINGS, 0, data, 7, output, sizeof(output)).status == 200 && !memcmp(pin, "012345", 6));
    data[1] = 'a'; before = mutations;
    assert(web_device_request(API_SETTINGS, 0, data, 7, output, sizeof(output)).status == 400 && mutations == before);
    save_status = STATUS_FLASH_WRITE_FAIL; data[0] = 5; data[1] = 60;
    assert(web_device_request(API_SETTINGS, 0, data, 2, output, sizeof(output)).status == 200 && output[1] == STATUS_FLASH_WRITE_FAIL);
    puts("Device metadata, setting validation, busy ownership and save outcomes passed");
}
