#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum { NRFX_NFC_FIELD_STATE_NONE, NRFX_NFC_FIELD_STATE_OFF,
    NRFX_NFC_FIELD_STATE_ON, NRFX_NFC_FIELD_STATE_UNKNOWN } nrfx_nfct_field_state_t;
typedef enum { NRFX_NFCT_STATE_ACTIVATED } nrfx_nfct_state_t;
typedef unsigned nrf_nfct_task_t;
typedef struct { unsigned evt_id; } nrfx_nfct_evt_t;
enum { NRFX_NFCT_EVT_FIELD_DETECTED, NRFX_NFCT_EVT_FIELD_LOST };
static struct { struct { void (*cb)(nrfx_nfct_evt_t *); } config;
    bool field_on; unsigned state; } m_nfct_cb;
static struct { unsigned timer; bool fieldevents_filter_active, is_hfclk_on,
    is_delayed, is_field_validation_pending; } m_timer_workaround;
static unsigned m_nfc_close_timer, detected, activated, stopped;
static bool physical_field;
#define NRFX_NFCT_TASK_ACTIVATE 1
#define NRF_NFCT_TASK_ACTIVATE NRFX_NFCT_TASK_ACTIVATE
#define NRF_NFCT_TASK_SENSE 2
#define NRFX_NFCT_RX_INT_MASK 1
#define NRFX_NFCT_TX_INT_MASK 2
#define NRFX_DRV_STATE_UNINITIALIZED 0
#define NFCT_IRQn 0
#define APP_TIMER_TICKS(n) (n)
#define NRFX_LOG_INFO(...) ((void)0)
#define NRFX_IRQ_DISABLE(n) ((void)(n))
#define NRFX_IRQ_PENDING_CLEAR(n) ((void)(n))
#define NRFX_NFCT_CB_HANDLE(cb,evt) do { if (cb) cb(&evt); } while (0)
static void app_timer_stop(unsigned timer) { (void)timer; }
static void app_timer_start(unsigned timer,unsigned ticks,void *context) { (void)timer;(void)ticks;(void)context; }
static void nrfx_timer_clear(unsigned *timer) { (void)timer; }
static void nrfx_timer_enable(unsigned *timer) { (void)timer; }
static void nrfx_timer_uninit(unsigned *timer) { (void)timer; stopped++; }
static void nrfx_nfct_disable(void) {}
static void nrf_nfct_int_disable(unsigned mask) { (void)mask; }
static void nrfx_nfct_frame_delay_max_set(bool value) { (void)value; }
static bool nrfx_nfct_field_check(void) { return physical_field; }
static void nrf_nfct_task_trigger(nrf_nfct_task_t task) { if(task==NRF_NFCT_TASK_ACTIVATE)activated++; }
/* DRIVER_CALLBACKS */

static void watch(nrfx_nfct_evt_t *event) {
    if(event->evt_id==NRFX_NFCT_EVT_FIELD_DETECTED)detected++;
}
static void clear_owner(void) {
    nrfx_nfct_uninit();
    assert(!m_nfct_cb.field_on && !m_timer_workaround.fieldevents_filter_active);
    assert(!m_timer_workaround.is_hfclk_on && !m_timer_workaround.is_delayed);
    assert(!m_timer_workaround.is_field_validation_pending);
}
int main(void) {
    physical_field=true;m_nfct_cb.config.cb=watch;
    for(unsigned cycle=0;cycle<20;cycle++) {
        /* ST25TB watches a field, then releases NFCT before clock activation. */
        nrfx_nfct_field_event_handler(NRFX_NFC_FIELD_STATE_ON);
        assert(detected==cycle*3+1 && m_timer_workaround.fieldevents_filter_active);
        clear_owner();
        /* Classic must get a fresh callback and perform the full activation. */
        nrfx_nfct_field_event_handler(NRFX_NFC_FIELD_STATE_ON);
        assert(detected==cycle*3+2);
        nrfx_nfct_state_force(NRFX_NFCT_STATE_ACTIVATED);
        m_timer_workaround.is_delayed=true;nrfx_nfct_activate_check();
        assert(activated==cycle*2+1 && m_timer_workaround.is_field_validation_pending);
        /* Cancel the second timer pass, then start again with another owner. */
        clear_owner();
        nrfx_nfct_field_event_handler(NRFX_NFC_FIELD_STATE_ON);
        assert(detected==cycle*3+3);
        nrfx_nfct_state_force(NRFX_NFCT_STATE_ACTIVATED);
        m_timer_workaround.is_delayed=true;nrfx_nfct_activate_check();
        assert(activated==cycle*2+2);
        nrfx_nfct_activate_check();
        assert(!m_timer_workaround.fieldevents_filter_active);
        physical_field=false;nrfx_nfct_field_event_handler(NRFX_NFC_FIELD_STATE_OFF);
        assert(!m_nfct_cb.field_on);physical_field=true;
        clear_owner();
    }
    assert(stopped==60);
}
