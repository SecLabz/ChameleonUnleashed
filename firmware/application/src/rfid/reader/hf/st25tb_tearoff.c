/* SPDX-License-Identifier: GPL-3.0-only */
#include "st25tb_tearoff.h"
#include "st25tb_reader.h"
#include "hf_radio.h"
#include "app_status.h"
#include "bsp_delay.h"
#include "bsp_wdt.h"
#include "nrf.h"
#include <stdlib.h>

#define ST25TB_TEAROFF_RETRIES 30u
#define ST25TB_TEAROFF_FAST_READS 6u
#define ST25TB_TEAROFF_SLOW_READS 2u
#define ST25TB_TEAROFF_READ_ATTEMPTS 3u
#define ST25TB_TEAROFF_SERVICE_INTERVAL_MS 50u
#define ST25TB_TEAROFF_VERIFY_WRITE_US 6000u
#define ST25TB_TEAROFF_STABLE_READ_PAUSE_MS 2000u
#define ST25TB_TEAROFF_DIRECT_DECREMENT_BELOW 0xffffu
#define ST25TB_TEAROFF_HIGH_NIBBLE_MASK 0xf0000000u
/* DWT counts core cycles at the existing 64 MHz CPU frequency. */
#define CPU_CYCLES_PER_MS 64000u
#define RECOVERY_LOG_INTERVAL_CYCLES (1000u * CPU_CYCLES_PER_MS)

typedef struct {
    const st25tb_tearoff_request_t *request;
    st25tb_tearoff_result_t result;
    uint32_t last_check;
    uint32_t last_recovery_log;
    uint64_t elapsed;
    bool uid_verified;
} run_t;

static uint32_t decode(const uint8_t value[4]) {
    return (uint32_t)value[0] | (uint32_t)value[1] << 8 |
           (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

static void encode(uint32_t value, uint8_t bytes[4]) {
    for (unsigned i = 0; i < 4; i++) bytes[i] = value >> (i * 8);
}

static void log_progress(run_t *run, uint8_t kind, uint32_t value,
        uint32_t delay_us) {
    if (run->request->progress)
        run->request->progress(kind, value, delay_us);
}

#define ST25TB_TEAROFF_START_US 130u

static uint32_t next_value(uint32_t current, bool randomness,
        uint32_t random_word) {
    uint32_t value = 0;
    if (current < ST25TB_TEAROFF_DIRECT_DECREMENT_BELOW) return current ? current - 1 : 0;
    for (int index = 31; index >= 0; index--) {
        if (!value && (current & (1u << index))) {
            value = UINT32_MAX >> (31 - index);
            index--;
        }
        if (index >= 0 && value && !(current & (1u << index))) {
            index++;
            value &= ~(1u << index);
            if (randomness && value < ST25TB_TEAROFF_HIGH_NIBBLE_MASK && index > 1)
                value ^= 1u << (random_word % (unsigned)index);
            return value;
        }
    }
    return value ? value - 1 : 0;
}

static bool expired(run_t *run) {
    bsp_wdt_feed();
    if (run->request->service) run->request->service();
    if (run->request->cancelled && run->request->cancelled()) {
        run->result.outcome = ST25TB_TEAROFF_CANCELLED;
        return true;
    }
    uint32_t now = DWT->CYCCNT;
    run->elapsed += (uint32_t)(now - run->last_check);
    run->last_check = now;
    if (run->elapsed >= (uint64_t)run->request->duration_ms * CPU_CYCLES_PER_MS ||
        hf_radio_expired()) {
        run->result.outcome = ST25TB_TEAROFF_DURATION_EXPIRED;
        return true;
    }
    return false;
}

static void recover(run_t *run) {
    uint32_t now = DWT->CYCCNT;
    if (run->result.valid &&
            (!run->last_recovery_log || now - run->last_recovery_log >= RECOVERY_LOG_INTERVAL_CYCLES)) {
        /* Reuse the established status event so older hosts keep streaming. */
        log_progress(run, ST25TB_TEAROFF_LOG_INITIAL, run->result.value, 0);
        run->last_recovery_log = now;
    }
    if (hf_radio_recover())
        st25tb_reset_inventory();
    /* A moved tag or a transient radio failure must re-establish identity. */
    run->uid_verified = false;
}

static bool read_block(run_t *run, uint32_t *value) {
    uint8_t bytes[4];
    for (;;) {
        for (unsigned attempt = 0; attempt < ST25TB_TEAROFF_READ_ATTEMPTS; attempt++) {
            if (expired(run)) return false;
            uint8_t stage = 0;
            uint16_t status = st25tb_tearoff_read(run->request->uid, !run->uid_verified,
                                               run->request->block, bytes, &stage);
            run->result.read_stage = stage;
            run->result.read_status = status;
            if (run->result.read_attempts < UINT8_MAX) run->result.read_attempts++;
            if (status == STATUS_HF_TAG_OK) {
                run->uid_verified = true;
                *value = decode(bytes);
                run->result.value = *value;
                run->result.valid = true;
                if (expired(run)) return false;
                run->result.outcome = ST25TB_TEAROFF_UNSUPPORTED;
                return true;
            }
            if (status == STATUS_BUSY) {
                run->result.outcome = ST25TB_TEAROFF_DURATION_EXPIRED;
                return false;
            }
            run->uid_verified = false;
        }
        if (expired(run)) return false;
        recover(run);
    }
}

static bool write_block(run_t *run, uint32_t value, uint32_t delay_us) {
    if (expired(run)) return false;
    uint8_t bytes[4];
    uint32_t cycles = 0;
    encode(value, bytes);
    bool written = st25tb_tearoff_write_selected(run->request->block, bytes,
                                               delay_us, &cycles);
    run->result.request_to_off_cycles = cycles;
    run->result.off_to_on_cycles = hf_radio_tearoff_last_off_cycles;
    if (!written) {
        run->uid_verified = false;
        run->result.outcome = ST25TB_TEAROFF_TRANSPORT_ERROR;
        return false;
    }
    return true;
}

static bool reset_field(run_t *run) {
    for (;;) {
        if (expired(run)) return false;
        if (hf_radio_tearoff_reset()) return true;
        recover(run);
    }
}

static bool wait_ms(run_t *run, unsigned milliseconds) {
    while (milliseconds) {
        if (expired(run)) {
            return false;
        }
        unsigned step = milliseconds > ST25TB_TEAROFF_SERVICE_INTERVAL_MS
            ? ST25TB_TEAROFF_SERVICE_INTERVAL_MS : milliseconds;
        uint32_t started = run->request->timing ? DWT->CYCCNT : 0;
        bsp_delay_ms(step);
        if (run->request->timing)
            run->result.pause_cycles += DWT->CYCCNT - started;
        milliseconds -= step;
    }
    return true;
}

static bool write_verify(run_t *run, uint32_t target,
        uint32_t *read_back) {
    for (unsigned i = 0; *read_back != target && i < ST25TB_TEAROFF_RETRIES; i++) {
        if (!write_block(run, target, ST25TB_TEAROFF_VERIFY_WRITE_US)) {
            if (run->result.outcome != ST25TB_TEAROFF_TRANSPORT_ERROR ||
                    !read_block(run, read_back)) return false;
            continue;
        }
        if (!read_block(run, read_back)) return false;
        if (*read_back == target)
            log_progress(run, ST25TB_TEAROFF_LOG_WRITE_VERIFIED, target, ST25TB_TEAROFF_VERIFY_WRITE_US);
    }
    return *read_back == target;
}

static bool check_stable(run_t *run, uint32_t expected,
        unsigned repeats, unsigned pause_ms, uint32_t *read_back) {
    for (unsigned i = 0; i < repeats; i++) {
        if (!wait_ms(run, pause_ms)) return false;
        if (!reset_field(run)) return false;
        if (!read_block(run, read_back)) return false;
        if (*read_back != expected) return false;
    }
    return true;
}

static bool target_stable(run_t *run, uint32_t *read_back) {
    bool near_max = run->request->target > (UINT32_MAX - 2u);
    if (!check_stable(run, run->request->target,
            near_max ? ST25TB_TEAROFF_FAST_READS : ST25TB_TEAROFF_SLOW_READS,
            0, read_back)) return false;
    return !near_max || check_stable(run, run->request->target,
        ST25TB_TEAROFF_SLOW_READS, ST25TB_TEAROFF_STABLE_READ_PAUSE_MS, read_back);
}

static bool consolidate(run_t *run, uint32_t current,
        uint32_t target, uint32_t *read_back) {
    uint32_t consolidation = target <= UINT32_MAX - 2 && current >= target + 2
                                 ? target + 2 : current;
    if (consolidation < 2 ||
            !write_verify(run, consolidation - 1, read_back)) return false;
    if (*read_back != (UINT32_MAX - 1u) || target == (UINT32_MAX - 2u)) {
        if (!write_verify(run, consolidation - 2, read_back)) return false;
    }
    if (target > (UINT32_MAX - 2u) && *read_back > (UINT32_MAX - 2u)) {
        if (!check_stable(run, *read_back,
                ST25TB_TEAROFF_FAST_READS, 0, read_back)) return false;
        if (!check_stable(run, *read_back,
                ST25TB_TEAROFF_SLOW_READS, ST25TB_TEAROFF_STABLE_READ_PAUSE_MS, read_back)) return false;
    }
    return true;
}

static void adjust_delay(uint32_t *delay_us,
        uint32_t start_us, uint8_t adjustment_us) {
    if (*delay_us > start_us) *delay_us -= adjustment_us;
}

st25tb_tearoff_result_t st25tb_tearoff_run(const st25tb_tearoff_request_t *request) {
    run_t run = {.request = request, .result = {.outcome = ST25TB_TEAROFF_UNSUPPORTED},
                 .last_check = DWT->CYCCNT};
    uint32_t current;
    if (!read_block(&run, &current)) return run.result;
    run.result.initial_valid = true;
    run.result.initial_value = current;
    if (request->progress)
        request->progress(ST25TB_TEAROFF_LOG_INITIAL, current, 0);
    if (current < ST25TB_TEAROFF_FLOOR) return run.result;
    if (request->reset_check) {
        if (request->target != current) return run.result;
        bool reset = reset_field(&run);
        run.result.off_to_on_cycles = hf_radio_tearoff_last_off_cycles;
        if (!reset) return run.result;
        uint32_t observed;
        if (!read_block(&run, &observed)) return run.result;
        if (observed == current) run.result.outcome = ST25TB_TEAROFF_SUCCESS;
        return run.result;
    }
    if (current == request->target) {
        if (target_stable(&run, &current)) {
            run.result.outcome = ST25TB_TEAROFF_SUCCESS;
            return run.result;
        }
        if (run.result.outcome != ST25TB_TEAROFF_UNSUPPORTED) return run.result;
        current = run.result.value;
        if (current < ST25TB_TEAROFF_FLOOR) return run.result;
    }
    uint32_t candidate = next_value(current, false, 0);
    uint32_t start_us = ST25TB_TEAROFF_START_US;
    uint32_t delay_us = start_us;
    uint32_t last_consolidated = current;
    bool trigger = true;
    for (;;) {
        if (candidate < ST25TB_TEAROFF_FLOOR) {
            run.result.outcome = ST25TB_TEAROFF_UNSUPPORTED;
            return run.result;
        }
        uint32_t observed;
        bool retrying = false;
        if (!write_block(&run, candidate, delay_us)) {
            if (run.result.outcome != ST25TB_TEAROFF_TRANSPORT_ERROR ||
                    !read_block(&run, &observed)) return run.result;
            retrying = true;
        } else if (!read_block(&run, &observed)) return run.result;
        if (observed == request->target) {
            if (target_stable(&run, &observed)) {
                run.result.outcome = ST25TB_TEAROFF_SUCCESS;
                return run.result;
            }
            if (run.result.outcome != ST25TB_TEAROFF_UNSUPPORTED) return run.result;
            observed = run.result.value;
        }
        if (observed > current) {
            if (observed >= (UINT32_MAX - 1u) ||
                    (observed >= 2 && observed - 2 >= request->target) ||
                    observed != last_consolidated ||
                    (observed & ST25TB_TEAROFF_HIGH_NIBBLE_MASK) > (current & ST25TB_TEAROFF_HIGH_NIBBLE_MASK)) {
                uint32_t consolidated = observed;
                bool verified = consolidate(&run, observed,
                                                                request->target, &consolidated);
                if (verified && consolidated == request->target) {
                    run.result.outcome = ST25TB_TEAROFF_SUCCESS;
                    return run.result;
                }
                if (run.result.outcome == ST25TB_TEAROFF_TAG_LOST ||
                        run.result.outcome == ST25TB_TEAROFF_CANCELLED ||
                        run.result.outcome == ST25TB_TEAROFF_DURATION_EXPIRED ||
                        run.result.outcome == ST25TB_TEAROFF_TRANSPORT_ERROR) return run.result;
                log_progress(&run, ST25TB_TEAROFF_LOG_ADVANCED,
                                   observed, delay_us);
                current = run.result.value;
                if (!retrying && observed != last_consolidated)
                    adjust_delay(&delay_us, start_us, request->adjustment_us);
                last_consolidated = observed;
                candidate = next_value(current, false, 0);
                trigger = true;
            }
        } else if (observed == candidate) {
            log_progress(&run, ST25TB_TEAROFF_LOG_CANDIDATE,
                               observed, delay_us);
            candidate = next_value(observed, trigger,
                                                 trigger ? (uint32_t)rand() : 0);
            trigger = !trigger;
            current = observed;
            if (!retrying)
                adjust_delay(&delay_us, start_us, request->adjustment_us);
        } else if (observed < candidate) {
            log_progress(&run, ST25TB_TEAROFF_LOG_LOWER,
                               observed, delay_us);
            candidate = next_value(observed, false, 0);
            current = observed;
            trigger = true;
            if (!retrying)
                adjust_delay(&delay_us, start_us, request->adjustment_us);
        }
        if (!retrying) delay_us++;
    }
}
