#include "rf_timeslot.h"
#include "nrf.h"
#include "nrf_sdh_soc.h"
#include "nrf_soc.h"
#include <stddef.h>

#define CPU_CYCLES_PER_US 64
#define EXTENSION_MARGIN_US 1000
#define MIN_SLOT_US 100
#define MAX_EXTENDED_SLOT_US 2000000u
#define INITIAL_EXTENDED_SLOT_US 5000
#define GRANT_TIMEOUT_US 100000u
#define RUN_TIMEOUT_US 250000u
#define CLOSE_TIMEOUT_US 100000u

static volatile bool active, finished, closed;
static volatile bool ran;
static bool extend_slot;
static uint32_t grant_start, granted_us, initial_us, target_us, extension_us, requested_us;
static void (*operation)(void);
static nrf_radio_signal_callback_return_param_t result;
static uint32_t run_started, request_started, last_run_cycles, last_grant_wait_cycles;

uint32_t rf_timeslot_last_run_cycles(void) { return last_run_cycles; }

uint32_t rf_timeslot_last_grant_wait_cycles(void) { return last_grant_wait_cycles; }

uint32_t rf_timeslot_granted_us(void) {
    return granted_us;
}

uint32_t rf_timeslot_deadline(void) {
    return grant_start + granted_us * CPU_CYCLES_PER_US;
}

static nrf_radio_signal_callback_return_param_t *signal_handler(uint8_t signal) {
    result.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;
    if (signal == NRF_RADIO_CALLBACK_SIGNAL_TYPE_START) {
        grant_start = DWT->CYCCNT;
        granted_us = initial_us;
        extension_us = target_us - granted_us;
    } else if (extend_slot && signal == NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_SUCCEEDED) {
        granted_us += requested_us;
        extension_us = requested_us;
    } else if (extend_slot && signal == NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_FAILED) {
        extension_us = requested_us / 2;
    } else {
        return &result;
    }
    if (extension_us > target_us - granted_us)
        extension_us = target_us - granted_us;
    if (extension_us > NRF_RADIO_LENGTH_MAX_US)
        extension_us = NRF_RADIO_LENGTH_MAX_US;
    if (extend_slot && extension_us >= NRF_RADIO_MINIMUM_TIMESLOT_LENGTH_EXTENSION_TIME_US &&
            (int32_t)(rf_timeslot_deadline() - DWT->CYCCNT) > EXTENSION_MARGIN_US * CPU_CYCLES_PER_US) {
        result.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_EXTEND;
        requested_us = extension_us;
        result.params.extend.length_us = requested_us;
        return &result;
    }
    operation();
    ran = true;
    result.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_END;
    return &result;
}

static void soc_handler(uint32_t event, void *context) {
    (void)context;
    if (!active) return;
    if (event == NRF_EVT_RADIO_SESSION_IDLE || event == NRF_EVT_RADIO_BLOCKED ||
            event == NRF_EVT_RADIO_CANCELED) finished = true;
    if (event == NRF_EVT_RADIO_SESSION_CLOSED) { closed = true; active = false; }
}
NRF_SDH_SOC_OBSERVER(rf_soc_observer, 0, soc_handler, NULL);

bool rf_timeslot_run(uint32_t microseconds, bool high_priority, bool extend, void (*work)(void)) {
    if (active || !work || microseconds < MIN_SLOT_US || microseconds > (extend ? MAX_EXTENDED_SLOT_US : NRF_RADIO_LENGTH_MAX_US))
        return false;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    run_started = DWT->CYCCNT;
    last_run_cycles = last_grant_wait_cycles = 0;
    operation = work;
    extend_slot = extend;
    target_us = microseconds;
    initial_us = microseconds > NRF_RADIO_LENGTH_MAX_US ? NRF_RADIO_LENGTH_MAX_US :
                 extend && microseconds > INITIAL_EXTENDED_SLOT_US ? INITIAL_EXTENDED_SLOT_US : microseconds;
    finished = closed = ran = false;
    result.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;
    active = true;
    if (sd_radio_session_open(signal_handler) != NRF_SUCCESS) {
        active = false;
        return false;
    }
    nrf_radio_request_t request = {0};
    request.request_type = NRF_RADIO_REQ_TYPE_EARLIEST;
    request.params.earliest.hfclk = NRF_RADIO_HFCLK_CFG_XTAL_GUARANTEED;
    request.params.earliest.priority = high_priority ? NRF_RADIO_PRIORITY_HIGH : NRF_RADIO_PRIORITY_NORMAL;
    request.params.earliest.length_us = initial_us;
    request.params.earliest.timeout_us = GRANT_TIMEOUT_US;
    request_started = DWT->CYCCNT;
    bool requested = sd_radio_request(&request) == NRF_SUCCESS;
    if (!requested) finished = true;
    uint32_t start = DWT->CYCCNT;
    while (!finished && DWT->CYCCNT - start < RUN_TIMEOUT_US * CPU_CYCLES_PER_US) __NOP();
    uint32_t wait_ended = DWT->CYCCNT;
    uint32_t status = sd_radio_session_close();
    start = DWT->CYCCNT;
    while ((status == NRF_SUCCESS || status == NRF_ERROR_BUSY) && !closed &&
            DWT->CYCCNT - start < CLOSE_TIMEOUT_US * CPU_CYCLES_PER_US) __NOP();
    if (!closed) NVIC_SystemReset();
    last_run_cycles = DWT->CYCCNT - run_started;
    if (ran) last_grant_wait_cycles = grant_start - request_started;
    else if (requested) last_grant_wait_cycles = wait_ended - request_started;
    return ran && closed;
}
