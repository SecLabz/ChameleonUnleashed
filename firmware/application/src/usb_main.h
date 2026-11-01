#ifndef USB_MAIN_H
#define USB_MAIN_H

#include <stdint.h>
#include <stdbool.h>

void usb_init(void);
void usb_start(void);
void usb_poll(void);
void usb_cdc_write(const void *p_buf, uint16_t length);
bool is_usb_working(void);
bool usb_hfclk_active(void);
bool usb_cdc_tx_ready(void);
void usb_cdc_request_end(void);
bool usb_cdc_tearoff_running(void);
void usb_cdc_tearoff_begin(void);
void usb_cdc_tearoff_end(void);
bool usb_cdc_tearoff_cancelled(void);
/* Safe in an RF callback: observes pending work without servicing USB. */
bool usb_work_pending(void);


#endif
