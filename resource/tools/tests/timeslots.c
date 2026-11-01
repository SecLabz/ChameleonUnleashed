#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t ret_code_t;
typedef void (*timeslot_callback_t)(void);
typedef struct {
    unsigned request_type;
    struct {
        struct {
            unsigned hfclk;
            unsigned priority;
            uint32_t length_us;
            uint32_t timeout_us;
        } earliest;
    } params;
} nrf_radio_request_t;
typedef struct {
    unsigned callback_action;
    union {
        struct {
            nrf_radio_request_t *p_next;
        } request;
        struct {
            uint32_t length_us;
        } extend;
    } params;
} nrf_radio_signal_callback_return_param_t;
typedef nrf_radio_signal_callback_return_param_t *(*radio_handler_t)(uint8_t signal);
typedef struct {
    void (*handler)(uint32_t event, void *context);
    void *context;
} observer_t;

#define NRF_SDH_SOC_OBSERVER(name, priority, handler, context) \
    static const observer_t name = {handler, context}
#define NRF_LOG_MODULE_REGISTER()
#define APP_ERROR_CHECK(code) assert((code) == NRF_SUCCESS)
#define APP_ERROR_CHECK_BOOL(condition) assert(condition)
#define NRF_LOG_PROCESS() pump()
#define __NOP() pump()
#define NVIC_DisableIRQ(irq) ((void)(irq))
#define NVIC_EnableIRQ(irq) ((void)(irq))
#define NVIC_SystemReset() assert(!"unexpected system reset")
#define NRF_RADIO_LENGTH_MAX_US 100000u
#define NRF_RADIO_EARLIEST_TIMEOUT_MAX_US 1000000u
#define NRF_RADIO_MINIMUM_TIMESLOT_LENGTH_EXTENSION_TIME_US 200u
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
static struct {
    uint32_t CYCCNT;
    uint32_t CTRL;
} cycles;
static struct {
    uint32_t DEMCR;
} debug;
#define DWT (&cycles)
#define CoreDebug (&debug)

enum {
    NRF_SUCCESS,
    NRF_ERROR_BUSY,
    NRF_ERROR_INVALID_PARAM,
    NRF_RADIO_REQ_TYPE_EARLIEST,
    NRF_RADIO_HFCLK_CFG_NO_GUARANTEE,
    NRF_RADIO_HFCLK_CFG_XTAL_GUARANTEED,
    NRF_RADIO_PRIORITY_HIGH,
    NRF_RADIO_PRIORITY_NORMAL,
    NRF_EVT_RADIO_SIGNAL_CALLBACK_INVALID_RETURN,
    NRF_EVT_RADIO_SESSION_IDLE,
    NRF_EVT_RADIO_SESSION_CLOSED,
    NRF_EVT_RADIO_BLOCKED,
    NRF_EVT_RADIO_CANCELED,
    NRF_RADIO_CALLBACK_SIGNAL_TYPE_START,
    NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_SUCCEEDED,
    NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_FAILED,
    NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE,
    NRF_RADIO_SIGNAL_CALLBACK_ACTION_EXTEND,
    NRF_RADIO_SIGNAL_CALLBACK_ACTION_END,
    RADIO_IRQn,
    TIMER0_IRQn,
    TIMER2_IRQn,
    GPIOTE_IRQn,
    MWU_IRQn,
    RTC1_IRQn
};
static radio_handler_t session;
static unsigned pending;
static unsigned deny_next;
static unsigned requests;
static unsigned closes;
static unsigned pumps;
static unsigned work_calls;
static uint32_t last_request_us;

static void pump(void);
static uint32_t sd_radio_session_open(radio_handler_t handler);
static uint32_t sd_radio_request(const nrf_radio_request_t *request);
static uint32_t sd_radio_session_close(void);

/* TIMESLOT IMPLEMENTATIONS */

static void dispatch(unsigned event) {
    // Nordic broadcasts each SoC event to both registered observers.
    m_sys_obs.handler(event, m_sys_obs.context);
    rf_soc_observer.handler(event, rf_soc_observer.context);
}

static void pump(void) {
    assert(++pumps < 10000);
    cycles.CYCCNT += 64u * 100u;
    unsigned event = pending;
    pending = 0;
    if (event == NRF_RADIO_CALLBACK_SIGNAL_TYPE_START) {
        assert(session);
        nrf_radio_signal_callback_return_param_t *reply = session(event);
        assert(reply->callback_action != NRF_RADIO_SIGNAL_CALLBACK_ACTION_EXTEND);
        if (reply->callback_action == NRF_RADIO_SIGNAL_CALLBACK_ACTION_END) {
            pending = NRF_EVT_RADIO_SESSION_IDLE;
        }
    } else if (event) {
        if (event == NRF_EVT_RADIO_SESSION_CLOSED) {
            session = NULL;
        }
        dispatch(event);
    }
}

static uint32_t sd_radio_session_open(radio_handler_t handler) {
    assert(!session && !pending);
    session = handler;
    return NRF_SUCCESS;
}

static uint32_t sd_radio_request(const nrf_radio_request_t *request) {
    assert(session);
    requests++;
    last_request_us = request->params.earliest.length_us;
    if (last_request_us < 100u || last_request_us > NRF_RADIO_LENGTH_MAX_US) {
        return NRF_ERROR_INVALID_PARAM;
    }
    assert(!pending);
    pending = deny_next ? deny_next : NRF_RADIO_CALLBACK_SIGNAL_TYPE_START;
    deny_next = 0;
    return NRF_SUCCESS;
}

static uint32_t sd_radio_session_close(void) {
    assert(session && !pending);
    closes++;
    // Deliver CLOSED asynchronously, after the caller begins waiting for it.
    pending = NRF_EVT_RADIO_SESSION_CLOSED;
    return NRF_SUCCESS;
}

static void work(void) {
    work_calls++;
}

static void denied_rf(unsigned event) {
    unsigned requests_before = requests;
    unsigned closes_before = closes;
    unsigned work_before = work_calls;
    deny_next = event;
    assert(!rf_timeslot_run(5000, false, false, work));
    assert(requests == requests_before + 1);
    assert(closes == closes_before + 1);
    assert(work_calls == work_before);
    assert(last_request_us == 5000 && !session && !pending);
}

static void legacy_retry(unsigned event) {
    unsigned requests_before = requests;
    unsigned work_before = work_calls;
    deny_next = event;
    request_timeslot(40000, work);
    assert(requests == requests_before + 2);
    assert(work_calls == work_before + 1);
    assert(last_request_us == 40000 && !session && !pending);
}

int main(void) {
    // No LF request has initialized the legacy duration yet.
    denied_rf(NRF_EVT_RADIO_BLOCKED);
    denied_rf(NRF_EVT_RADIO_CANCELED);
    legacy_retry(NRF_EVT_RADIO_BLOCKED);
    legacy_retry(NRF_EVT_RADIO_CANCELED);
    // A past LF operation must not retry its stale duration for an HF session.
    denied_rf(NRF_EVT_RADIO_BLOCKED);
    denied_rf(NRF_EVT_RADIO_CANCELED);
    unsigned work_before = work_calls;
    assert(rf_timeslot_run(5000, true, false, work));
    assert(work_calls == work_before + 1 && !session && !pending);
    // The split legacy API retains ownership until its own CLOSED event.
    timeslot_start(40);
    assert(session && last_request_us == 40000);
    timeslot_stop();
    assert(!session && !pending);
    denied_rf(NRF_EVT_RADIO_BLOCKED);
    denied_rf(NRF_EVT_RADIO_CANCELED);
    unsigned requests_before = requests;
    dispatch(NRF_EVT_RADIO_BLOCKED);
    dispatch(NRF_EVT_RADIO_CANCELED);
    dispatch(NRF_EVT_RADIO_SESSION_CLOSED);
    assert(requests == requests_before);
    assert(rf_timeslot_run(5000, false, false, work));
    request_timeslot(40000, work);
    assert(!session && !pending);
    return 0;
}
