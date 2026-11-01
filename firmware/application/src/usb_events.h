#pragma once
#include "nrf_usbd.h"

/* nRF52840 event registers follow their interrupt bits in contiguous words. */
_Static_assert(NRF_USBD_EVENT_DATAEP == NRF_USBD_EVENT_USBRESET + 24u * sizeof(uint32_t),
               "Unexpected USBD event register layout");

static inline bool usb_irq_work_pending(void) {
    if (NRF_USBD->ENABLE != USBD_ENABLE_ENABLE_Enabled) return false;
    /* A timeslot may preempt the USB ISR before it queues the cleared event. */
    if (NVIC_GetActive(USBD_IRQn)) return true;
    if (!NVIC_GetPendingIRQ(USBD_IRQn)) return false;
    uint32_t enabled = nrf_usbd_int_enable_get() & ~USBD_INTEN_SOF_Msk;
    unsigned event = NRF_USBD_EVENT_USBRESET;
    for (; enabled; enabled >>= 1, event += sizeof(uint32_t)) {
        if ((enabled & 1u) && nrf_usbd_event_check((nrf_usbd_event_t)event)) return true;
    }
    return false;
}
