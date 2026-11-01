#include "hf/hf_iq.h"
#include "rf_workspace.h"

/* ADC residuals are at most 15 per axis, or 340 mixed units at scale 16.
 * Decoder differences span at most 95 input samples, hence below 32768;
 * circular prefix sums therefore preserve bounded differences after wrap.
 */
static union {
    struct {
        uint16_t i[8193];
        uint16_t q[8193];
        uint8_t samples[8193];
    } b;
    struct {
        uint16_t i[5633];
        uint16_t q[5633];
        uint8_t samples[11265];
    } v;
    struct {
        int16_t adc[RF_LF_ADC_BANKS][RF_LF_ADC_SAMPLES];
        uint16_t samples[RF_LF_CAPTURE_SAMPLES];
    } lf;
    uint32_t sniff[RF_HF_SNIFF_BYTES / sizeof(uint32_t)];
} workspace;
uint16_t *hf_iq_integral_i;
uint16_t *hf_iq_integral_q;

uint8_t *hf_iq_prepare(bool vicinity) {
    hf_iq_integral_i = vicinity ? workspace.v.i : workspace.b.i;
    hf_iq_integral_q = vicinity ? workspace.v.q : workspace.b.q;
    return vicinity ? workspace.v.samples : workspace.b.samples;
}

uint16_t *rf_lf_workspace(void) {
    return workspace.lf.samples;
}

void *rf_hf_sniff_workspace(void) {
    return workspace.sniff;
}

int16_t *rf_lf_adc_workspace(unsigned bank) {
    return workspace.lf.adc[bank];
}
