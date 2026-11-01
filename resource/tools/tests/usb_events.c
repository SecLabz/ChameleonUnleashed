#include "usb_events.h"
#include <assert.h>
#include <string.h>

test_usbd_t test_usbd;

int main(void) {
    for (unsigned bit = 0; bit < 25; bit++) {
        memset(&test_usbd, 0, sizeof(test_usbd));
        test_usbd.events[bit] = 1;
        test_usbd.INTENSET = 1u << bit;
        test_usbd.pending = 1;
        assert(!usb_irq_work_pending()); /* Disabled peripheral, stale events. */
        test_usbd.ENABLE = 1;
        assert(usb_irq_work_pending() == (bit != 21));
        assert(test_usbd.events[bit] == 1); /* Only the USB driver may clear it. */
        test_usbd.INTENSET = 0;
        assert(!usb_irq_work_pending());
    }
    test_usbd.INTENSET = (1u << 21) | (1u << 23);
    test_usbd.events[21] = test_usbd.events[23] = 1;
    assert(usb_irq_work_pending()); /* SOF cannot hide a concurrent SETUP. */
    test_usbd.pending = 0;
    assert(!usb_irq_work_pending());
    memset(test_usbd.events, 0, sizeof(test_usbd.events));
    test_usbd.active = 1;
    assert(usb_irq_work_pending()); /* USB ISR preempted before queue publication. */
}
