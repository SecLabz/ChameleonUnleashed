#pragma once
#include <stddef.h>
#include <stdint.h>

/* IRQ-safe cancellation; parser cleanup remains with the transport callbacks. */
void usb_tearoff_cancel(void);
void usb_tearoff_reset_parser(void);
void usb_tearoff_end(void);
void usb_tearoff_receive(const uint8_t *data, size_t length);
