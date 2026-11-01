#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/rfid/reader/hf/hf_reader.c"
#include "../src/rfid/reader/hf/st25tb_tearoff_command.c"
#include "utils/dataframe.h"

mock_dwt_t mock_dwt;
mock_core_debug_t mock_core_debug;
volatile bool m_is_field_on;
static data_source_t source = DATA_SOURCE_USB;
static int device_mode = DEVICE_MODE_READER;
static bool capture, advertising = true, connected, open_ok = true;
static unsigned stops, starts, opens, closes;
static uint16_t read_status;
static bool usb_active = true;
bool usb_hfclk_active(void) { return usb_active; }

bool web_tearoff_running(void) { return false; }

bool web_tearoff_cancelled(void) { assert(false); return false; }

void web_tearoff_service(void) { assert(false); }

void web_tearoff_progress(uint8_t kind, uint32_t value, uint16_t delay_us) {
    (void)kind; (void)value; (void)delay_us; assert(false);
}

data_source_t data_frame_source(void) { return source; }

bool is_usb_working(void) { return true; }

bool usb_cdc_tx_ready(void) { return true; }

void usb_cdc_write(const void *bytes, uint16_t size) { (void)bytes; (void)size; }

void usb_poll(void) {}

void usb_cdc_tearoff_begin(void) { assert(false); }

void usb_cdc_tearoff_end(void) { assert(false); }

bool usb_cdc_tearoff_cancelled(void) { return false; }

void bsp_wdt_feed(void) {}
data_frame_tx_t *data_frame_make(uint16_t cmd, uint16_t status,
        uint16_t length, uint8_t *data) {
    (void)cmd; (void)status; (void)length; (void)data;
    assert(false);
    return NULL;
}

bool advertising_stop_checked(void) {
    stops++;
    if (connected || !advertising) return false;
    advertising = false;
    return true;
}

void advertising_start(bool erase_bonds) {
    assert(!erase_bonds && !advertising && opens == closes);
    advertising = true;
    starts++;
}

int get_device_mode(void) { return device_mode; }

bool nfca_session_active(void) { return capture; }

void hf_radio_depth(bool deeper) { assert(!deeper); }

bool hf_radio_open(bool vicinity) {
    assert(!vicinity);
    opens++;
    return open_ok;
}

void hf_radio_keep(void) { assert(false); }

bool hf_radio_close(void) { closes++; return true; }

void hf_radio_tearoff_deadline(uint32_t ms) { (void)ms; assert(false); }

void hf_radio_tearoff_timing(bool enabled) { (void)enabled; assert(false); }

hf_tearoff_timing_t hf_radio_tearoff_timing_result(void) {
    assert(false);
    return (hf_tearoff_timing_t){0};
}

st25tb_tearoff_result_t st25tb_tearoff_run(const st25tb_tearoff_request_t *request) {
    (void)request;
    assert(false);
    return (st25tb_tearoff_result_t){0};
}

uint16_t nfcb_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    (void)command; (void)data; (void)output; (void)size;
    assert(false);
    return STATUS_HF_ERR_STAT;
}

uint16_t st25tb_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    (void)data;
    assert(command == DATA_CMD_ST25TB_READ_BLOCK || command == DATA_CMD_ST25TB_INFO ||
           command == DATA_CMD_ST25TB_DUMP);
    if (read_status) return read_status;
    memset(output, 0x31, 4);
    *size = 4;
    return STATUS_HF_TAG_OK;
}

uint16_t nfcv_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    (void)command; (void)data; (void)output; (void)size;
    assert(false);
    return STATUS_HF_ERR_STAT;
}

static uint16_t read_block(uint8_t block, uint16_t length) {
    uint8_t request[9] = {0}, output[HF_READER_OUTPUT_BYTES];
    request[8] = block;
    uint16_t size;
    uint16_t status = hf_reader_command(DATA_CMD_ST25TB_READ_BLOCK, length,
        request, output, sizeof(output), &size);
    assert(opens == closes);
    return status;
}

static uint16_t read_command(uint16_t command) {
    uint8_t request[9] = {0}, output[HF_READER_OUTPUT_BYTES];
    uint16_t length = command == DATA_CMD_ST25TB_INFO ? 0 :
                      command == DATA_CMD_ST25TB_DUMP ? 8 : 9;
    uint16_t size;
    uint16_t status = hf_reader_command(command, length, length ? request : NULL,
        output, sizeof(output), &size);
    assert(opens == closes);
    return status;
}

int main(void) {
    assert(read_block(16, 9) == STATUS_PAR_ERR);
    assert(read_block(0, 8) == STATUS_PAR_ERR);
    device_mode = 0;
    assert(read_block(0, 9) == STATUS_DEVICE_MODE_ERROR);
    device_mode = DEVICE_MODE_READER;
    capture = true;
    assert(read_block(0, 9) == STATUS_BUSY);
    capture = false;
    m_is_field_on = true;
    assert(read_block(0, 9) == STATUS_BUSY);
    m_is_field_on = false;
    assert(stops == 0 && opens == 0);

    assert(read_block(0, 9) == STATUS_HF_TAG_OK);
    assert(advertising && starts == 1);
    read_status = STATUS_HF_ERR_CRC;
    assert(read_block(0, 9) == STATUS_HF_ERR_CRC);
    assert(advertising && starts == 2);
    read_status = 0;
    open_ok = false;
    assert(read_block(0, 9) == STATUS_HF_ERR_STAT);
    assert(advertising && starts == 3);
    open_ok = true;

    connected = true;
    assert(read_block(0, 9) == 0);
    assert(starts == 3);
    connected = false;
    advertising = false;
    assert(read_block(0, 9) == 0);
    assert(!advertising && starts == 3);
    advertising = true;
    source = DATA_SOURCE_BLE;
    unsigned previous_stops = stops;
    assert(read_block(0, 9) == 0);
    assert(stops == previous_stops + 1 && starts == 4);
    source = DATA_SOURCE_USB;
    const uint16_t commands[] = {DATA_CMD_ST25TB_INFO, DATA_CMD_ST25TB_READ_BLOCK,
                                 DATA_CMD_ST25TB_DUMP};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        for (unsigned failure = 0; failure < 3; failure++) {
            previous_stops = stops;
            unsigned previous_starts = starts;
            read_status = failure == 1 ? STATUS_HF_ERR_CRC : 0;
            open_ok = failure != 2;
            uint16_t expected = failure == 2 ? STATUS_HF_ERR_STAT : read_status;
            assert(read_command(commands[i]) == expected);
            assert(advertising && stops == previous_stops + 1 &&
                   starts == previous_starts + 1);
        }
        read_status = 0; open_ok = true; connected = true;
        unsigned previous_starts = starts;
        assert(read_command(commands[i]) == 0);
        assert(advertising && starts == previous_starts);
        connected = false;
        usb_active = false;
        previous_stops = stops;
        assert(read_command(commands[i]) == 0);
        assert(advertising && stops == previous_stops && starts == previous_starts);
        usb_active = true;
    }
    puts("ST25TB USB read cleanup, BLE ownership and USB clock guards passed");
}

uint16_t nfcv_unlock_privacy(const uint8_t password[4]) {
    (void)password;
    assert(false);
    return STATUS_HF_ERR_STAT;
}

uint16_t nfcv_write_block(const uint8_t *data, uint8_t *output, uint16_t *size) {
    (void)data;
    (void)output;
    (void)size;
    assert(false);
    return STATUS_HF_ERR_STAT;
}

uint16_t nfcv_raw(uint16_t length, const uint8_t *data, uint8_t *output, uint16_t *size, bool *complete) {
    (void)complete;
    (void)length;
    (void)data;
    (void)output;
    (void)size;
    assert(false);
    return STATUS_HF_ERR_STAT;
}
