#include "rf_workspace.h"
#include "hf/hf_iq.h"
#include <assert.h>
#include <string.h>

int main(void) {
    uint16_t *samples = rf_lf_workspace();
    int16_t *first = rf_lf_adc_workspace(0);
    int16_t *second = rf_lf_adc_workspace(1);
    for (unsigned i = 0; i < RF_LF_ADC_SAMPLES; i++) {
        first[i] = 123;
        second[i] = -456;
    }
    for (unsigned i = 0; i < RF_LF_CAPTURE_SAMPLES; i++) {
        samples[i] = i;
    }
    for (unsigned i = 0; i < RF_LF_ADC_SAMPLES; i++) {
        assert(first[i] == 123 && second[i] == -456);
    }
    for (unsigned i = 0; i < RF_LF_CAPTURE_SAMPLES; i++) {
        assert(samples[i] == i);
    }
    memset(rf_hf_sniff_workspace(), 0x5a, RF_HF_SNIFF_BYTES);
    uint8_t *iq = hf_iq_prepare(false);
    memset(iq, 0x12, 8193);
    for (unsigned i = 0; i < 8193; i++) {
        hf_iq_integral_i[i] = 0x3456;
        hf_iq_integral_q[i] = 0x789a;
    }
    for (unsigned i = 0; i < 8193; i++) {
        assert(iq[i] == 0x12 && hf_iq_integral_i[i] == 0x3456 && hf_iq_integral_q[i] == 0x789a);
    }
}
