#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/rfid/reader/hf/hf_reader.c"
#include "../src/rfid/reader/hf/st25tb_tearoff_command.c"

static bool web_running;
bool usb_hfclk_active(void) { return true; }

bool web_tearoff_running(void) { return web_running; }

bool web_tearoff_cancelled(void) { return false; }

void web_tearoff_service(void) {}

void web_tearoff_progress(uint8_t kind, uint32_t value, uint16_t delay_us) {
    tearoff_progress(kind, value, delay_us);
}

mock_dwt_t mock_dwt;
mock_core_debug_t mock_core_debug;
static bool live_active;
static uint8_t last_progress_kind;
static data_source_t source = DATA_SOURCE_USB;
static bool advertising = true, connected;
static unsigned stops, starts;
data_source_t data_frame_source(void) { return source; }

bool is_usb_working(void) { return true; }

bool usb_cdc_tx_ready(void) { return true; }

void usb_cdc_write(const void *bytes, uint16_t size) { (void)bytes; (void)size; }

void usb_poll(void) {}

void usb_cdc_tearoff_begin(void) { live_active = true; }

void usb_cdc_tearoff_end(void) { live_active = false; }

bool usb_cdc_tearoff_cancelled(void) { return false; }

void bsp_wdt_feed(void) {}
data_frame_tx_t *data_frame_make(uint16_t cmd, uint16_t status,
        uint16_t length, uint8_t *data) {
    (void)cmd; (void)status; (void)length;
    last_progress_kind = data[0];
    static uint8_t bytes[16];
    static data_frame_tx_t frame = {.buffer = bytes};
    frame.length = 16;
    return &frame;
}
volatile bool m_is_field_on;
static unsigned opened, closed, runs;
static bool open_ok = true, close_ok = true;
static int mode = DEVICE_MODE_READER;
static bool session_active;
static uint32_t deadline_ms;
static bool use_transcript_response;
static st25tb_tearoff_result_t transcript_response;

int get_device_mode(void) { return mode; }

bool nfca_session_active(void) { return session_active; }

void hf_radio_depth(bool deeper) { (void)deeper; }

bool hf_radio_open(bool vicinity) { assert(!vicinity); opened++; return open_ok; }

bool advertising_stop_checked(void) {
    stops++;
    if (connected || !advertising) return false;
    advertising = false;
    return true;
}

void advertising_start(bool erase_bonds) {
    assert(!erase_bonds && !advertising && opened == closed);
    advertising = true;
    starts++;
}

void hf_radio_keep(void) { assert(false); }

bool hf_radio_close(void) { closed++; return close_ok; }

void hf_radio_tearoff_deadline(uint32_t milliseconds) { deadline_ms = milliseconds; }
static bool timing_enabled;
void hf_radio_tearoff_timing(bool enabled) { timing_enabled = enabled; }

hf_tearoff_timing_t hf_radio_tearoff_timing_result(void) {
    if (!timing_enabled) return (hf_tearoff_timing_t){0};
    return (hf_tearoff_timing_t){.timeslot_cycles = 64000, .grant_wait_cycles = 32000,
        .dma_cycles = 16000, .decode_cycles = 8000, .exchanges = 3,
        .writes = 1, .resets = 0};
}

st25tb_tearoff_result_t st25tb_tearoff_run(const st25tb_tearoff_request_t *request) {
    assert(request->block == 5 && request->target == 0xfffffffeu);
    runs++;
    if (use_transcript_response) return transcript_response;
    request->progress(ST25TB_TEAROFF_LOG_INITIAL, 0xffffffffu, 0);
    return (st25tb_tearoff_result_t){.outcome = ST25TB_TEAROFF_SUCCESS,
                                      .valid = true, .value = 0xfffffffeu};
}

uint16_t nfcb_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    (void)command; (void)data; (void)output; (void)size;
    assert(false);
    return STATUS_HF_ERR_STAT;
}

uint16_t st25tb_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    (void)command; (void)data; (void)output; (void)size;
    assert(false);
    return STATUS_HF_ERR_STAT;
}

uint16_t nfcv_reader_command(uint16_t command, const uint8_t *data,
        uint8_t *output, uint16_t *size) {
    (void)command; (void)data; (void)output; (void)size;
    assert(false);
    return STATUS_HF_ERR_STAT;
}

int main(void) {
    uint8_t request[ST25TB_TEAROFF_REQUEST_BYTES] = {
        ST25TB_TEAROFF_FORMAT, 0xd0, 2, 0x33, 1, 2, 3, 4, 5,
        5, 0xfe, 0xff, 0xff, 0xff, 0, 25, 0x40, 0x77, 0x1b, 0
    };
    uint8_t result[HF_READER_OUTPUT_BYTES];
    uint16_t size;
    const unsigned invalid_fields[] = {0, 1, 2, 3, 9, 14, 15, 19};
    for (unsigned i = 0; i < sizeof(invalid_fields) / sizeof(invalid_fields[0]); i++) {
        unsigned index = invalid_fields[i];
        uint8_t original = request[index];
        request[index] = index == 9 ? 4 : 255;
        assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
            request, result, sizeof(result), &size) == STATUS_PAR_ERR);
        assert(size == 0 && opened == 0);
        request[index] = original;
    }
    for (uint8_t old_version = 1; old_version <= 7; old_version++) {
        request[0] = old_version;
        assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
            request, result, sizeof(result), &size) == STATUS_PAR_ERR);
    }
    request[0] = ST25TB_TEAROFF_FORMAT;
    for (uint8_t product = 0x18; product <= 0x1b; product++) {
        request[3] = product;
        assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
            request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
        assert(size == ST25TB_TEAROFF_RESULT_BYTES);
    }
    request[3] = 0x1f;
    unsigned previous_opens = opened;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_PAR_ERR);
    assert(opened == previous_opens);
    request[3] = 0x33;
    request[0] = ST25TB_TEAROFF_FORMAT;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request) - 1,
        request, result, sizeof(result), &size) == STATUS_PAR_ERR);
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, 7, &size) == STATUS_PAR_ERR);
    request[16] = 0xf4;
    request[17] = 1;
    request[18] = request[19] = 0;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(deadline_ms == 500);
    request[16] = 0x40;
    request[17] = 0x77;
    request[18] = 0x1b;

    mode = 0;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_DEVICE_MODE_ERROR);
    mode = DEVICE_MODE_READER;
    m_is_field_on = true;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_BUSY);
    m_is_field_on = false;
    session_active = true;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_BUSY);
    session_active = false;

    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(deadline_ms == 1800000 && size == ST25TB_TEAROFF_RESULT_BYTES);
    assert(result[0] == ST25TB_TEAROFF_SUCCESS && result[1] == 1 &&
           result[2] == 1 && result[3] == 0 && result[4] == 0xfe);
    assert(last_progress_kind == ST25TB_TEAROFF_LOG_INITIAL && !live_active);
    for (unsigned i = 24; i < size; i++) assert(result[i] == 0);
    assert(advertising && starts == stops && opened == closed);

    request[0] = ST25TB_TEAROFF_FORMAT | 1;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(size == ST25TB_TEAROFF_RESULT_BYTES && result[3] == 2);
    assert(result[28] == 0 && result[29] == 0xfa && result[30] == 0 &&
           result[52] == 3 && result[56] == 1 && result[60] == 0);

    memset(&transcript_response, 0, sizeof(transcript_response));
    transcript_response.outcome = ST25TB_TEAROFF_SUCCESS;
    transcript_response.valid = true;
    transcript_response.value = 0xfffffffeu;
    transcript_response.initial_valid = true;
    transcript_response.initial_value = 0xffffffffu;
    transcript_response.read_stage = 4;
    use_transcript_response = true;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(result[3] == 3 && result[8] == 0xff && result[20] == 4);
    use_transcript_response = false;

    close_ok = false;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(result[2] == 0);
    open_ok = false;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(result[0] == ST25TB_TEAROFF_TRANSPORT_ERROR && result[1] == 0);
    open_ok = close_ok = true;
    request[0] = ST25TB_TEAROFF_FORMAT;
    request[14] = 4;
    request[15] = 0;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(result[0] == ST25TB_TEAROFF_SUCCESS);
    assert(advertising && starts == stops && opened == closed);

    connected = true;
    unsigned previous_starts = starts;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(starts == previous_starts);
    connected = false;
    advertising = false;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(!advertising && starts == previous_starts);
    advertising = true;
    source = DATA_SOURCE_BLE;
    unsigned previous_stops = stops;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_NOT_IMPLEMENTED);
    assert(stops == previous_stops && starts == previous_starts);
    web_running = true;
    assert(hf_reader_command(DATA_CMD_ST25TB_TEAROFF, sizeof(request),
        request, result, sizeof(result), &size) == STATUS_HF_TAG_OK);
    assert(size == 64 && !live_active && stops == previous_stops);
    puts("ST25TB tearoff command passed");
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
