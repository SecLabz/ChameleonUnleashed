#include "web_tearoff.h"
#include "bytes.h"
#include "st25tb_tearoff.h"
#include "st25tb_reader.h"
#include "app_timer.h"
#include "app_status.h"
#include "data_cmd.h"
#include "hf_reader.h"
#include "hf_radio.h"
#include "usb_main.h"
#include "services/control.h"
#include "utils/dataframe.h"
#include <string.h>

enum { IDLE, PENDING, RUNNING, DONE };
enum {
    START_BLOCK = ST25TB_UID_BYTES,
    START_TARGET = START_BLOCK + 1,
    START_BYTES = START_TARGET + sizeof(uint32_t),
    STATE_CURRENT_VALID = 1u << 0,
    STATE_CLEANUP_OK = 1u << 1,
    STATE_CURRENT = 4,
    STATE_TARGET = 8,
    STATE_WRITES = 12,
    STATE_DELAY = 16,
    STATE_ELAPSED = 18,
};
#define NETWORK_SERVICE_INTERVAL_MS 50u
static struct {
    uint8_t request[ST25TB_TEAROFF_REQUEST_BYTES];
    uint8_t state, outcome, block, flags;
    uint16_t delay_us;
    uint32_t current, target, writes, serviced, elapsed_ticks;
    uint16_t elapsed_seconds;
    bool cancel;
} job;

bool web_tearoff_start(const uint8_t *request, size_t size) {
    if (job.state == PENDING || job.state == RUNNING || size != START_BYTES || !request ||
            request[0] != ST25TB_UID_FAMILY || request[1] != ST25TB_UID_MANUFACTURER ||
            request[ST25TB_UID_PRODUCT_OFFSET] != ST25TB_PRODUCT_512_AT ||
            (request[START_BLOCK] != ST25TB_COUNTER_FIRST_BLOCK &&
             request[START_BLOCK] != ST25TB_COUNTER_LAST_BLOCK)) return false;
    uint32_t target = read_le32(request + START_TARGET);
    if (target < ST25TB_TEAROFF_FLOOR || target > UINT32_MAX - 1u) return false;
    memset(&job, 0, sizeof(job));
    job.state = PENDING;
    job.block = request[START_BLOCK];
    job.target = target;
    job.request[ST25TB_TEAROFF_REQUEST_FORMAT] =
        ST25TB_TEAROFF_FORMAT | ST25TB_TEAROFF_REQUEST_TIMING;
    memcpy(job.request + ST25TB_TEAROFF_REQUEST_UID, request, START_BYTES);
    job.request[ST25TB_TEAROFF_REQUEST_MODE] = ST25TB_TEAROFF_MODE_RUN;
    job.request[ST25TB_TEAROFF_REQUEST_ADJUSTMENT] = ST25TB_TEAROFF_DEFAULT_ADJUSTMENT_US;
    write_le32(job.request + ST25TB_TEAROFF_REQUEST_DURATION,
        ST25TB_TEAROFF_DEFAULT_DURATION_MS);
    return true;
}

bool web_tearoff_active(void) { return job.state == PENDING || job.state == RUNNING; }

bool web_tearoff_running(void) { return job.state == RUNNING; }

void web_tearoff_cancel(void) { job.cancel = true; }

bool web_tearoff_cancelled(void) { return job.cancel; }

void web_tearoff_progress(uint8_t kind, uint32_t value, uint16_t delay_us) {
    job.current = value;
    job.flags |= STATE_CURRENT_VALID;
    if (kind != ST25TB_TEAROFF_LOG_INITIAL) job.delay_us = delay_us;
}

void web_tearoff_service(void) {
    uint32_t now = app_timer_cnt_get();
    uint32_t delta = app_timer_cnt_diff_compute(now, job.serviced);
    if (delta < APP_TIMER_TICKS(NETWORK_SERVICE_INTERVAL_MS)) return;
    job.elapsed_ticks += delta;
    job.serviced = now;
    usb_poll();
    data_frame_process_ble_control();
    web_session_poll();
}

void web_tearoff_poll(void) {
    if (job.state != PENDING) return;
    if (job.cancel) { job.state = DONE; job.outcome = ST25TB_TEAROFF_CANCELLED; return; }
    job.state = RUNNING;
    job.serviced = app_timer_cnt_get();
    uint8_t result[HF_READER_OUTPUT_BYTES];
    uint16_t size = 0;
    uint16_t status = hf_reader_command(DATA_CMD_ST25TB_TEAROFF,
        sizeof(job.request), job.request, result, sizeof(result), &size);
    job.elapsed_ticks += app_timer_cnt_diff_compute(app_timer_cnt_get(), job.serviced);
    uint32_t seconds = job.elapsed_ticks / APP_TIMER_TICKS(1000);
    job.elapsed_seconds = seconds > UINT16_MAX ? UINT16_MAX : seconds;
    job.state = DONE;
    if (status != STATUS_HF_TAG_OK || size != ST25TB_TEAROFF_RESULT_BYTES) {
        job.outcome = ST25TB_TEAROFF_TRANSPORT_ERROR;
        return;
    }
    job.outcome = result[ST25TB_TEAROFF_RESULT_OUTCOME];
    job.writes = read_le32(result + ST25TB_TEAROFF_RESULT_WRITES);
    if (result[ST25TB_TEAROFF_RESULT_VALID]) {
        job.current = read_le32(result + ST25TB_TEAROFF_RESULT_VALUE);
        job.flags |= STATE_CURRENT_VALID;
    }
    if (result[ST25TB_TEAROFF_RESULT_CLEANUP]) job.flags |= STATE_CLEANUP_OK;
}

size_t web_tearoff_state(uint8_t output[WEB_TEAROFF_STATE_BYTES]) {
    memset(output, 0, WEB_TEAROFF_STATE_BYTES);
    output[0] = job.state;
    output[1] = job.outcome;
    output[2] = job.block;
    output[3] = job.flags;
    write_le32(output + STATE_CURRENT, job.current);
    write_le32(output + STATE_TARGET, job.target);
    write_le32(output + STATE_WRITES, job.state == RUNNING
        ? hf_radio_tearoff_timing_result().writes : job.writes);
    write_le16(output + STATE_DELAY, job.delay_us);
    uint32_t seconds = job.state == RUNNING
        ? (job.elapsed_ticks + app_timer_cnt_diff_compute(app_timer_cnt_get(), job.serviced)) /
          APP_TIMER_TICKS(1000)
        : job.elapsed_seconds;
    if (seconds > UINT16_MAX) seconds = UINT16_MAX;
    write_le16(output + STATE_ELAPSED, seconds);
    return WEB_TEAROFF_STATE_BYTES;
}
