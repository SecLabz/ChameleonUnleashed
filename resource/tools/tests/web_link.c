#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nfc_mf0_ntag.h"
#include "rf_workspace.h"
typedef unsigned ret_code_t;

#define APP_TIMER_DEF(name) static const unsigned name = 1
#define APP_TIMER_TICKS(ms) (ms)
#define APP_ERROR_CHECK(error) assert((error) == 0)
#define ASSERT(condition) assert(condition)
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_ERROR(...) ((void)0)
#define NRF_ERROR_INVALID_PARAM 1
#define SLEEP_DELAY_MS_BUTTON_CLICK 4000
#define NRFX_POWER_USB_STATE_DISCONNECTED 0
#define RGB_LIST_NUM 8
#define RGB_BLUE 2
#define TAG_FIELD_LED_OFF() ((void)0)
#define DEVICE_MODE_TAG 2
#define DEVICE_MODE_READER 1
#define m_app_sleep_timer 2

static tag_slot_config_t slotConfig;
static bool slot_sense_held, slot_sense_requested[3];
static bool sensing_14a, sensing_15, sensing_st25, sensing_lf, sleep_armed, timer_armed;
static bool g_is_ble_connected, m_is_field_on;
bool g_is_tag_emulating, g_usb_led_marquee_enable;
static unsigned color, led_mask, mode = DEVICE_MODE_TAG, restores, timer_ticks;
static uint8_t original[4500], session_byte;
static uint16_t original_crc = 0x1234;
static tag_data_buffer_t m_tag_data_hf = {sizeof(original), original, &original_crc};
static nfc_tag_mf0_ntag_information_t *m_tag_information;
static tag_specific_type_t m_tag_type;
static uint32_t led_pins[8] = {0, 1, 2, 3, 4, 5, 6, 7};

/* LINK_STATE */
#define web_link_image (*(web_link_image_t *)rf_hf_sniff_workspace())

static unsigned app_timer_stop(unsigned timer) {
    assert(timer == m_web_link_timer);
    timer_armed = false;
    return 0;
}
static unsigned app_timer_start(unsigned timer, unsigned ticks, void *context) {
    assert(!context);
    if (timer == m_web_link_timer) {
        timer_armed = true;
        timer_ticks = ticks;
    } else {
        assert(timer == m_app_sleep_timer);
        sleep_armed = true;
    }
    return 0;
}
static void sleep_timer_stop(void) { sleep_armed = false; }
static unsigned nrfx_power_usbstatus_get(void) { return NRFX_POWER_USB_STATE_DISCONNECTED; }
/* SLEEP_CALLBACK */
static unsigned get_device_mode(void) { return mode; }
uint8_t tag_emulation_get_slot(void) { return slotConfig.active_slot; }
static unsigned get_color_by_slot(unsigned slot) { assert(slot == 3); return 1; }
static void set_slot_light_color(unsigned value) { color = value; }
static void light_up_by_slot(void) { led_mask = 1u << tag_emulation_get_slot(); }
static void rgb_marquee_stop(void) {}
static uint32_t *hw_get_led_array(void) { return led_pins; }
static void nrf_gpio_pin_set(unsigned pin) { assert(pin < 8); led_mask |= 1u << pin; }
void nfc_tag_14a_sense_switch(bool enable) { sensing_14a = enable; }
static void nfc_tag_15_sense_switch(bool enable) { sensing_15 = enable; }
static void nfc_st25tb_sense_switch(bool enable) { sensing_st25 = enable; }
static void lf_tag_125khz_sense_switch(bool enable) { sensing_lf = enable; }
static bool st25tb_loaded(void) { return true; }
static void tag_mode_enter(void) { mode = DEVICE_MODE_TAG; m_is_field_on = false; }
static int get_information_size_by_tag_type(tag_specific_type_t type);
static void nfc_tag_mf0_ntag_state_handler(uint8_t *data, uint16_t bits) { (void)data; (void)bits; }
static void nfc_tag_mf0_ntag_reset_handler(void) {}
nfc_tag_14a_coll_res_reference_t *nfc_tag_mf0_ntag_get_coll_res(void) { return NULL; }
void nfc_tag_14a_set_handler(nfc_tag_14a_handler_t *handler) { assert(handler->cb_state); }
void nfc_tag_14a_set_reset_enable(bool enable) { assert(enable); }
static void ulc_3des_init(void) { assert(false); }
/* NTAG_CALLBACKS */

static int restore_loader(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    assert(!sensing_14a && buffer == &m_tag_data_hf);
    restores++;
    if (type == TAG_TYPE_NTAG_213) return nfc_tag_mf0_ntag_data_loadcb(type, buffer);
    return buffer->length;
}
static tag_datas_loadcb_t get_data_loadcb_from_tag_type(tag_specific_type_t type) {
    return type == TAG_TYPE_UNDEFINED ? NULL : restore_loader;
}
static int session_save(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    assert(type == TAG_TYPE_SLIX || type == TAG_TYPE_SLIX_L || type == TAG_TYPE_ST25TB512_AT);
    assert(!sensing_15 && !sensing_st25);
    buffer->buffer[0] = session_byte;
    return 80;
}
static tag_datas_savecb_t get_data_savecb_from_tag_type(tag_specific_type_t type) {
    (void)type;
    return session_save;
}
static bool sync_snapshot_buffer(tag_specific_type_t type, tag_data_buffer_t *buffer);
/* LINK_CALLBACKS */

static void check_ndef(void) {
    const uint8_t *memory = web_link_image.memory[0];
    const uint8_t *ndef = memory + 16;
    const char uri[] = "seclabz.github.io/ChameleonUnleashed/app/";
    assert(!memcmp(memory + 12, (uint8_t[]){0xe1, 0x10, 0x12, 0x0f}, 4));
    assert(ndef[0] == 3 && ndef[1] == 46 && ndef[2] == 0xd1);
    assert(ndef[3] == 1 && ndef[4] == 42 && ndef[5] == 'U' && ndef[6] == 4);
    assert(!memcmp(ndef + 7, uri, sizeof(uri) - 1) && ndef[48] == 0xfe);
    assert(m_tag_information == (void *)&web_link_image && m_tag_type == TAG_TYPE_NTAG_213);
    assert(m_tag_information->config.mode_block_write == NFC_TAG_MF0_NTAG_WRITE_DENIED);
    assert(!memcmp(web_link_image.res_coll.uid, memory, 3));
    assert(!memcmp(web_link_image.res_coll.uid + 3, memory + 4, 4));
    assert((0x88 ^ memory[0] ^ memory[1] ^ memory[2]) == memory[3]);
    assert((memory[4] ^ memory[5] ^ memory[6] ^ memory[7]) == memory[8]);
    assert(web_link_image.memory[41][3] == 0xff);
    assert(!memcmp(web_link_image.memory[NTAG213_PAGES + 1],
                   (uint8_t[]){0, 4, 4, 2, 1, 0, 15, 3}, 8));
}

static void exercise_slot(tag_specific_type_t type, bool enabled, unsigned exit) {
    memset(original, 0x5a, sizeof(original));
    session_byte = 0xc7;
    slotConfig.active_slot = 3;
    slotConfig.slots[3].tag_hf = type;
    slotConfig.slots[3].enabled_hf = enabled;
    slotConfig.slots[3].tag_lf = TAG_TYPE_EM410X;
    slotConfig.slots[3].enabled_lf = true;
    tag_slot_config_t before = slotConfig;
    nfc_tag_mf0_ntag_information_t *ntag = (void *)original;
    ntag->config.mode_block_write = NFC_TAG_MF0_NTAG_WRITE_SHADOW;
    uint8_t expected[sizeof(original)];
    memcpy(expected, original, sizeof(expected));
    if (type == TAG_TYPE_SLIX || type == TAG_TYPE_SLIX_L || type == TAG_TYPE_ST25TB512_AT)
        expected[0] = session_byte;
    tag_emulation_sense_run();
    g_usb_led_marquee_enable = true;
    tag_emulation_web_link_start();
    assert(tag_emulation_web_link_active() && timer_armed && timer_ticks == 30000);
    assert(sensing_14a && !sensing_15 && !sensing_st25 && !sensing_lf);
    assert(color == RGB_BLUE && led_mask == 255 && !g_usb_led_marquee_enable);
    assert(!sleep_armed);
    check_ndef();
    // Field loss must not replace the independent 30-second shortcut deadline.
    sleep_timer_start(3000);
    assert(!sleep_armed && timer_armed && timer_ticks == 30000);
    tag_emulation_web_link_poll();
    assert(tag_emulation_web_link_active());
    unsigned restored = restores;
    if (exit == 0) {
        assert(tag_emulation_web_link_stop());
    } else if (exit == 1) {
        web_link_timeout(NULL);
        assert(tag_emulation_web_link_active()); // RF restoration stays in main.
        tag_emulation_web_link_poll();
    } else if (exit == 2) {
        g_is_ble_connected = true;
        tag_emulation_web_link_poll();
        g_is_ble_connected = false;
    } else {
        tag_emulation_sense_end();
    }
    assert(!tag_emulation_web_link_active() && !timer_armed);
    assert(restores == restored + (type != TAG_TYPE_UNDEFINED));
    assert(!memcmp(&before, &slotConfig, sizeof(before)));
    assert(!memcmp(original, expected, sizeof(original)) && original_crc == 0x1234);
    assert(color == 1 && led_mask == (1u << 3));
    if (exit == 3) assert(!sensing_14a && !sensing_15 && !sensing_st25 && !sensing_lf);
    else {
        assert(sensing_lf);
        assert(sensing_15 == (enabled && (type == TAG_TYPE_SLIX || type == TAG_TYPE_SLIX_L)));
        assert(sensing_st25 == (enabled && type == TAG_TYPE_ST25TB512_AT));
        assert(sensing_14a == (enabled && type != TAG_TYPE_UNDEFINED &&
            type != TAG_TYPE_SLIX && type != TAG_TYPE_SLIX_L && type != TAG_TYPE_ST25TB512_AT));
    }
    if (type == TAG_TYPE_NTAG_213) {
        assert(m_tag_information == (void *)original);
        assert(nfc_tag_mf0_ntag_data_savecb(type, &m_tag_data_hf) == 0); // shadow stays shadow
    }
    assert(!tag_emulation_web_link_stop());
    tag_emulation_sense_end();
}

int main(void) {
    const tag_specific_type_t types[] = {TAG_TYPE_NTAG_213, TAG_TYPE_MIFARE_1024,
        TAG_TYPE_SLIX, TAG_TYPE_SLIX_L, TAG_TYPE_ST25TB512_AT, TAG_TYPE_UNDEFINED};
    for (unsigned type = 0; type < sizeof(types) / sizeof(types[0]); type++)
        for (unsigned exit = 0; exit < 4; exit++)
            for (unsigned enabled = 0; enabled < 2; enabled++)
                exercise_slot(types[type], enabled, exit);
    g_is_ble_connected = true;
    tag_emulation_web_link_start();
    assert(!tag_emulation_web_link_active());
    g_is_ble_connected = false;
    slot_sense_held = true;
    tag_emulation_web_link_start();
    assert(!tag_emulation_web_link_active());
}
