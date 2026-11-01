#pragma once

#include <stdint.h>

/* Reader commands and ST25TB emulation windows own this storage exclusively.
 * LF ADC banks coexist with decoded LF samples; HF operations reuse their
 * combined storage. Callers cannot retain contents after their operation ends. */
#define RF_LF_CAPTURE_SAMPLES 12288
uint16_t *rf_lf_workspace(void);

#define RF_LF_ADC_SAMPLES 2048u
#define RF_LF_ADC_BANKS 2u
int16_t *rf_lf_adc_workspace(unsigned bank);

#define RF_HF_SNIFF_BYTES 49152u
void *rf_hf_sniff_workspace(void);
