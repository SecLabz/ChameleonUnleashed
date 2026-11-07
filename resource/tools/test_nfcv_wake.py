"""Exercise NFC-V radio windows, field-loss recovery and clock drift."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from native_test import function, run_c

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'firmware/application/src'
TIMING_INCLUDE = f'#include "{SRC / "rfid/iso15693/timing.h"}"\n'


class NfcvWakeTests(unittest.TestCase):
    def test_long_addressed_requests_track_clock_drift_and_require_crc(self):
        source = (SRC / 'rfid/nfctag/hf/nfcv_radio.c').read_text().replace('NFCV_RAM_FUNCTION ', '')
        fixture = (ROOT / 'resource/tools/tests/nfcv_receiver.c').read_text()
        fixture = fixture.replace('/* RECEIVER */', function(source, 'receive_frame_or_slot'))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'receiver.c').write_text(fixture)
            subprocess.run([
                'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(SRC / 'rfid'),
                str(path / 'receiver.c'), str(SRC / 'rfid/iso15693/frame.c'),
                '-o', str(path / 'receiver'),
            ], check=True)
            subprocess.run([str(path / 'receiver')], check=True, timeout=15)

    def test_advertising_pause_distinguishes_stopped_from_errors(self):
        source = (SRC / 'ble_main.c').read_text()
        run_c(TIMING_INCLUDE + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define NRF_SUCCESS 0
#define NRF_ERROR_INVALID_STATE 8
typedef uint32_t ret_code_t;
static bool g_is_ble_connected, rf_only_boot;
static struct { unsigned adv_handle; } m_advertising = {7};
static unsigned status, calls;
static uint32_t sd_ble_gap_adv_stop(unsigned handle) {
    assert(handle == 7); calls++; return status;
}
''' + function(source, 'advertising_pause') + r'''
int main(void) {
    bool resume = true;
    status = NRF_SUCCESS;
    assert(advertising_pause(&resume) && resume && calls == 1);
    status = NRF_ERROR_INVALID_STATE;
    assert(advertising_pause(&resume) && !resume && calls == 2);
    status = 0x3004; /* Invalid advertising handle is not an idle radio. */
    assert(!advertising_pause(&resume) && !resume && calls == 3);
    g_is_ble_connected = true; resume = true;
    assert(!advertising_pause(&resume) && !resume && calls == 3);
    rf_only_boot = true;
    assert(!advertising_pause(&resume) && !resume && calls == 3);
    g_is_ble_connected = false; m_advertising.adv_handle = 0;
    assert(advertising_pause(&resume) && !resume && calls == 3);
    return 0;
}
''')

    def test_rf_only_boot_keeps_long_windows_without_starting_advertising(self):
        source = (SRC / 'rfid/nfctag/hf/nfcv_radio.c').read_text()
        run_c(TIMING_INCLUDE + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
static bool enabled = true, g_is_ble_connected, idle, resume, rf_only = true, field;
static bool usb_pending;
static bool usb_work_pending(void) { return usb_pending; }
static unsigned feeds, resumes, windows, pauses;
static uint32_t granted_us;
static bool ble_slave_rf_only(void) { return rf_only; }
static bool nfcv_radio_field_present(void) { return field; }
static bool advertising_pause(bool *restore) { pauses++; *restore = resume; return idle; }
static void bsp_wdt_feed(void) { feeds++; }
static void advertising_start(bool erase) { assert(!erase); resumes++; }
static void poll_window(void) {}
static bool rf_timeslot_run(uint32_t us, bool high, bool extend, void (*work)(void)) {
    bool discovery = !rf_only && !field;
    assert(high == (!g_is_ble_connected && !discovery));
    assert(extend == (!discovery && idle) && work == poll_window);
    granted_us = us; windows++; return true;
}
''' + function(source, 'nfcv_radio_poll') + r'''
int main(void) {
    /* Advertising never started at RF wake: long enough for full-memory replies,
     * including the 43 ms high-rate and 174 ms low-rate SLIX security response. */
    assert(!usb_work_pending());
    idle = true;
    nfcv_radio_poll();
    assert(granted_us == 1500000 && feeds == 1 && !resumes);
    rf_only = false; field = true; resume = true;
    nfcv_radio_poll(); assert(resumes == 1);
    /* Full boots without an RF field must leave advertising running even if
     * it could have been paused. Stop/start on each poll starves discovery. */
    field = false;
    nfcv_radio_poll();
    assert(granted_us == 25000 && pauses == 2 && resumes == 1 && feeds == 2);
    field = true;
    idle = resume = false;
    nfcv_radio_poll(); assert(granted_us == 25000 && resumes == 1);
    g_is_ble_connected = true;
    nfcv_radio_poll(); assert(granted_us == 10000 && feeds == 2);
    /* Quiet HTTP transfers must not request a fresh RF grant per packet.
     * A real field still gets the same bounded radio service during USB. */
    usb_pending = true; field = false;
    nfcv_radio_poll(); assert(windows == 5 && pauses == 4);
    field = true;
    nfcv_radio_poll(); assert(windows == 6 && granted_us == 10000);
    enabled = false;
    nfcv_radio_poll(); assert(windows == 6);
    return 0;
}
''')

    def test_usb_pending_includes_tcp_work_after_usb_events_are_drained(self):
        source = (SRC / 'usb_main.c').read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
static bool queued, irq, tcp;
static bool tud_task_event_ready(void) { return queued; }
static bool usb_irq_work_pending(void) { return irq; }
static bool network_work_pending(void) { return tcp; }
''' + function(source, 'usb_work_pending') + r'''
int main(void) {
    assert(!usb_work_pending());
    tcp = true; assert(usb_work_pending());
    tcp = false; queued = true; assert(usb_work_pending());
    queued = false; irq = true; assert(usb_work_pending());
    irq = false; assert(!usb_work_pending());
    return 0;
}
''')

    def test_loss_during_receive_resets_before_the_next_reader(self):
        source = (SRC / 'rfid/nfctag/hf/nfcv_radio.c').read_text()
        run_c(TIMING_INCLUDE + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define NFCV_ACTIVATE_TIMEOUT_CYCLES 64000u
#define NFCV_FIELD_LOSS_GUARD_CYCLES 128000u
#define NFCT_FIELDPRESENT_FIELDPRESENT_Msk 1u
#define NFCT_NFCTAGSTATE_NFCTAGSTATE_Idle 3u
static struct { uint32_t CYCCNT; } dwt;
static struct { uint32_t NFCTAGSTATE, FIELDPRESENT; } nfct;
#define NRF_NFCT (&nfct)
static bool enabled = true, transient_loss;
static uint32_t restore_field_at;
static void *tick(void) {
    dwt.CYCCNT += 640;
    if (restore_field_at && dwt.CYCCNT >= restore_field_at) {
        nfct.FIELDPRESENT = 1;
        restore_field_at = 0;
    }
    return &dwt;
}
#define DWT ((typeof_dwt *)tick())
typedef struct { uint32_t CYCCNT; } typeof_dwt;
static uint32_t window_end, last_field_cycles, sense_started_cycles;
static unsigned resets, senses, receives;
static struct { unsigned byte_count; } frame;
static unsigned response;
static uint32_t rf_timeslot_deadline(void) { return 1500000u * 64u; }
static void activate(void) { nfct.NFCTAGSTATE = 3; }
static void sense(void) { senses++; }
static void nfcv_session_field_lost(void) { resets++; }
static uint32_t __get_PRIMASK(void) { return 0; }
static void __disable_irq(void) {}
static void __set_PRIMASK(uint32_t saved) { assert(saved == 0); }
static bool receive_frame_or_slot(void) {
    receives++;
    last_field_cycles = dwt.CYCCNT;
    nfct.FIELDPRESENT = 0;
    if (transient_loss && receives == 1) restore_field_at = dwt.CYCCNT + 64000;
    return false;
}
static bool nfcv_session_handle(void *f, void *r) { (void)f; (void)r; assert(0); return false; }
static bool nfcv_session_next_slot(void *r) { (void)r; assert(0); return false; }
static void send_response(void) { assert(0); }
static bool usb_work_pending(void) { return false; }
''' + function(source, 'poll_window') + r'''
int main(void) {
    nfct.NFCTAGSTATE = 3; nfct.FIELDPRESENT = 1;
    poll_window();
    assert(receives == 1 && resets == 1 && senses == 1);
    assert(dwt.CYCCNT < 140000); /* Confirm within about 2 ms, not next grant. */
    /* A loss already present when a grant starts also resets the session. */
    dwt.CYCCNT += 200000;
    poll_window(); assert(resets == 2 && senses == 2);
    /* A short modulation/detector dip must not reset SELECTED/QUIET state. */
    dwt.CYCCNT = last_field_cycles = receives = resets = senses = 0;
    nfct.FIELDPRESENT = 1; transient_loss = true;
    poll_window();
    assert(receives == 2 && resets == 1 && senses == 1);
    assert(dwt.CYCCNT > 192000 && dwt.CYCCNT < 205000);
    /* Disabled emulation must not touch the session. */
    enabled = false; poll_window(); assert(resets == 1 && receives == 2);
    return 0;
}
''')

    def test_pending_usb_yields_after_a_complete_rf_exchange(self):
        source = (SRC / 'rfid/nfctag/hf/nfcv_radio.c').read_text()
        run_c(TIMING_INCLUDE + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define NFCT_FIELDPRESENT_FIELDPRESENT_Msk 1u
#define NFCT_NFCTAGSTATE_NFCTAGSTATE_Idle 3u
static struct {
    uint32_t CYCCNT;
} dwt;
static struct {
    uint32_t NFCTAGSTATE;
    uint32_t FIELDPRESENT;
} nfct = {3, 1};
#define DWT (&dwt)
#define NRF_NFCT (&nfct)
static bool enabled = true;
static uint32_t window_end;
static uint32_t last_field_cycles;
static uint32_t sense_started_cycles;
static uint32_t irq;
static unsigned receives;
static unsigned replies;
static struct {
    unsigned byte_count;
} frame = {5};
static unsigned response;

static uint32_t rf_timeslot_deadline(void) {
    return 1500000u * 64u;
}

static void activate(void) {
    assert(0);
}

static void sense(void) {
    assert(0);
}

static void nfcv_session_field_lost(void) {
    assert(0);
}

static uint32_t __get_PRIMASK(void) {
    return irq;
}

static void __disable_irq(void) {
    irq = 1;
}

static void __set_PRIMASK(uint32_t saved) {
    irq = saved;
}

static bool receive_frame_or_slot(void) {
    assert(irq);
    receives++;
    return true;
}

static bool nfcv_session_handle(void *f, void *r) {
    assert(irq && f == &frame && r == &response);
    return true;
}

static bool nfcv_session_next_slot(void *r) {
    (void)r;
    assert(0);
    return false;
}

static void send_response(void) {
    assert(irq);
    replies++;
}

static bool usb_work_pending(void) {
    assert(!irq && replies == 1);
    return true;
}
''' + function(source, 'poll_window') + r'''
int main(void) {
    poll_window();
    assert(receives == 1 && replies == 1 && !irq);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
