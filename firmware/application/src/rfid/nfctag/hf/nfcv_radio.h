#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ultra backend: bounded polling with physical HF_ANT_SEL transmission. */
bool nfcv_radio_start(void);
/* Stop atomically and leave the antenna switch at its inactive level. */
void nfcv_radio_stop(void);
void nfcv_radio_poll(void);
bool nfcv_radio_field_present(void);
