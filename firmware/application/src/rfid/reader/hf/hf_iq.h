#ifndef HF_IQ_H
#define HF_IQ_H
#include <stdbool.h>
#include <stdint.h>
extern uint16_t *hf_iq_integral_i, *hf_iq_integral_q;
/* RX samples and integrals coexist; B/V workspaces share exclusive ownership. */
uint8_t *hf_iq_prepare(bool vicinity);

static inline uint16_t hf_iq_accumulate(uint16_t previous, float value) {
    /* Keep this intrinsic with the firmware's -fno-builtin build. */
    int32_t scaled = (int32_t)(value * 16 + __builtin_copysignf(0.5f, value));
    return (uint16_t)(previous + scaled);
}

static inline float hf_iq_difference(const uint16_t *values, unsigned begin, unsigned end) {
    int32_t delta = (uint16_t)(values[end] - values[begin]);
    if (delta >= 32768) delta -= 65536;
    return delta / 16.0f;
}
#endif
