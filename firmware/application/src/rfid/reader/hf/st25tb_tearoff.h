#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Request: format 0xA0 with bit 0 for timing, UID[8], block, target LE[4], mode,
 * adjustment_us, duration_ms LE[4]. Progress: kind, delay LE[2], value LE[4].
 * Result: outcome, valid, cleanup, flags (bit 0: initial valid; bit 1: timing),
 * last value LE[4], initial
 * value LE[4], request-to-off cycles LE[4], off-to-on cycles LE[4], read stage,
 * read attempts, read status LE[2], then ten LE32 timing fields. Timing fields
 * are zero unless requested. An out-of-frame cancel marker stops the run.
 * Byte 14 is zero for a normal run; value 4 performs the read-only reset check. */
#define ST25TB_TEAROFF_REQUEST_BYTES 20u
#define ST25TB_TEAROFF_FORMAT 0xa0u
#define ST25TB_TEAROFF_MIN_DURATION_MS 500u
#define ST25TB_TEAROFF_DEFAULT_DURATION_MS (30u * 60u * 1000u)
#define ST25TB_TEAROFF_MAX_DURATION_MS (120u * 60u * 1000u)
#define ST25TB_TEAROFF_DEFAULT_ADJUSTMENT_US 22u
#define ST25TB_TEAROFF_MAX_ADJUSTMENT_US 100u
#define ST25TB_TEAROFF_FLOOR 0x1000u
#define ST25TB_TEAROFF_RESULT_BYTES 64u
#define ST25TB_TEAROFF_PROGRESS_BYTES 7u
#define ST25TB_TEAROFF_PROGRESS_STATUS 0x79u
#define ST25TB_TEAROFF_TIMING_FIELDS 10u

/* Fixed byte offsets in the existing command format. */
enum {
    ST25TB_TEAROFF_REQUEST_FORMAT = 0,
    ST25TB_TEAROFF_REQUEST_UID = 1,
    ST25TB_TEAROFF_REQUEST_BLOCK = 9,
    ST25TB_TEAROFF_REQUEST_TARGET = 10,
    ST25TB_TEAROFF_REQUEST_MODE = 14,
    ST25TB_TEAROFF_REQUEST_ADJUSTMENT = 15,
    ST25TB_TEAROFF_REQUEST_DURATION = 16,
};
enum {
    ST25TB_TEAROFF_MODE_RUN = 0,
    ST25TB_TEAROFF_MODE_RESET_CHECK = 4,
    ST25TB_TEAROFF_REQUEST_TIMING = 1u << 0,
};
enum {
    ST25TB_TEAROFF_RESULT_OUTCOME = 0,
    ST25TB_TEAROFF_RESULT_VALID = 1,
    ST25TB_TEAROFF_RESULT_CLEANUP = 2,
    ST25TB_TEAROFF_RESULT_FLAGS = 3,
    ST25TB_TEAROFF_RESULT_VALUE = 4,
    ST25TB_TEAROFF_RESULT_INITIAL_VALUE = 8,
    ST25TB_TEAROFF_RESULT_REQUEST_TO_OFF = 12,
    ST25TB_TEAROFF_RESULT_OFF_TO_ON = 16,
    ST25TB_TEAROFF_RESULT_READ_STAGE = 20,
    ST25TB_TEAROFF_RESULT_READ_ATTEMPTS = 21,
    ST25TB_TEAROFF_RESULT_READ_STATUS = 22,
    ST25TB_TEAROFF_RESULT_TIMING = 24,
    ST25TB_TEAROFF_RESULT_WRITES = 56,
};
enum {
    ST25TB_TEAROFF_RESULT_INITIAL_VALID = 1u << 0,
    ST25TB_TEAROFF_RESULT_HAS_TIMING = 1u << 1,
};

typedef enum {
    ST25TB_TEAROFF_SUCCESS,
    ST25TB_TEAROFF_CANCELLED,
    ST25TB_TEAROFF_DURATION_EXPIRED,
    ST25TB_TEAROFF_UNSUPPORTED,
    ST25TB_TEAROFF_TAG_LOST,
    ST25TB_TEAROFF_TRANSPORT_ERROR,
} st25tb_tearoff_outcome_t;

typedef enum {
    ST25TB_TEAROFF_LOG_ADVANCED,
    ST25TB_TEAROFF_LOG_CANDIDATE,
    ST25TB_TEAROFF_LOG_LOWER,
    ST25TB_TEAROFF_LOG_WRITE_VERIFIED,
    ST25TB_TEAROFF_LOG_INITIAL,
} st25tb_tearoff_log_kind_t;

typedef void (*st25tb_tearoff_progress_t)(uint8_t kind, uint32_t value,
                                          uint16_t delay_us);

typedef struct {
    const uint8_t *uid;
    uint8_t block;
    uint32_t target;
    bool reset_check;
    uint32_t duration_ms;
    uint8_t adjustment_us;
    bool timing;
    st25tb_tearoff_progress_t progress;
    bool (*cancelled)(void);
    void (*service)(void);
} st25tb_tearoff_request_t;

typedef struct {
    st25tb_tearoff_outcome_t outcome;
    bool valid;
    uint32_t value;
    uint32_t request_to_off_cycles;
    uint32_t off_to_on_cycles;
    uint16_t read_status;
    uint8_t read_stage;
    uint8_t read_attempts;
    bool initial_valid;
    uint32_t initial_value;
    uint32_t pause_cycles;
} st25tb_tearoff_result_t;

st25tb_tearoff_result_t st25tb_tearoff_run(const st25tb_tearoff_request_t *request);

/* Adapter for the existing serial/web command; the caller validates buffer sizes. */
uint16_t st25tb_tearoff_command(const uint8_t *data, uint8_t *output, uint16_t *size);
