#include "nfca_session.h"
#include "app_timer.h"
#include "rc522.h"
#include "desfire_reader.h"

static bool active, prepared, reading;
static uint32_t updated;

void nfca_session_close(void) {
    desfire_reader_invalidate();
    if (active) pcd_14a_reader_antenna_off();
    active = prepared = reading = false;
}

void nfca_session_open(void) {
    nfca_session_close();
    active = true;
    updated = app_timer_cnt_get();
}

void nfca_session_read_open(void) {
    nfca_session_open();
    reading = true;
}

bool nfca_session_reading(void) {
    return active && reading;
}

bool nfca_session_active(void) {
    return active;
}

bool nfca_session_ready(void) {
    return active && prepared;
}

bool nfca_session_finish(void) {
    if (!active) return false;
    prepared = true;
    updated = app_timer_cnt_get();
    return true;
}

void nfca_session_poll(void) {
    // Main-loop cleanup; no timer callback performs SPI or changes RF state.
    if (active && app_timer_cnt_diff_compute(app_timer_cnt_get(), updated) >= APP_TIMER_TICKS(2000)) {
        nfca_session_close();
    }
}
