"""USB work detection must preserve control phases and ignore periodic SOF."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UsbEventsTests(unittest.TestCase):
    def test_enabled_events_without_acknowledging_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'nrf_usbd.h').write_text('''
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint32_t ENABLE, INTENSET, active, pending, events[25];} test_usbd_t;
extern test_usbd_t test_usbd;
#define NRF_USBD (&test_usbd)
#define USBD_ENABLE_ENABLE_Enabled 1u
#define USBD_INTEN_SOF_Msk (1u << 21)
#define USBD_IRQn 39
#define NVIC_GetActive(irq) test_usbd.active
#define NVIC_GetPendingIRQ(irq) test_usbd.pending
typedef enum {NRF_USBD_EVENT_USBRESET=0x100, NRF_USBD_EVENT_DATAEP=0x160} nrf_usbd_event_t;
static inline uint32_t nrf_usbd_int_enable_get(void) {return test_usbd.INTENSET;}
static inline bool nrf_usbd_event_check(nrf_usbd_event_t event) {
    return test_usbd.events[((unsigned)event-0x100)/4] != 0;
}
''')
            binary = directory / 'events'
            subprocess.run([
                'cc', '-std=c11', '-O3', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(directory),
                '-I', str(ROOT / 'firmware/application/src'),
                str(ROOT / 'resource/tools/tests/usb_events.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
