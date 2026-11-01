#include "usb_tearoff.h"
#include "usb_main.h"
#include "utils/dataframe.h"
#include "data_cmd.h"

static volatile bool active, cancelled;
static uint8_t matched;

void usb_cdc_request_end(void) { cancelled = false; matched = 0; }

bool usb_cdc_tearoff_running(void) { return active; }

void usb_cdc_tearoff_begin(void) { active = true; }

bool usb_cdc_tearoff_cancelled(void) {
    usb_poll();
    return cancelled;
}

void usb_tearoff_cancel(void) { if (active) cancelled = true; }

void usb_tearoff_reset_parser(void) {
    matched = 0;
    if (!active) cancelled = false;
}

void usb_tearoff_end(void) { active = cancelled = false; }

/* Preserve framed payloads; recognize the existing CLI marker only between frames. */
void usb_tearoff_receive(const uint8_t *data, size_t length) {
    static const uint8_t marker[] = {3, 'S', 'T', '2', '5', 'S', 'T', 'O', 'P'};
    for (size_t i = 0; i < length; i++) {
        uint8_t value = data[i];
        if (data_frame_usb_inside_frame()) {
            data_frame_receive(&value, 1);
            continue;
        }
        if (value == marker[matched]) {
            matched++;
            if (matched == sizeof(marker)) {
                if (active || data_frame_usb_pending(DATA_CMD_ST25TB_TEAROFF))
                    cancelled = true;
                matched = 0;
            }
            continue;
        }
        for (uint8_t i = 0; i < matched; i++) data_frame_receive(&marker[i], 1);
        matched = 0;
        if (value == marker[0]) matched = 1;
        else data_frame_receive(&value, 1);
    }
}
