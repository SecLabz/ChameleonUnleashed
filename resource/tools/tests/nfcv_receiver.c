#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "iso15693/frame.h"
#include "iso15693/commands.h"
#include "iso15693/ppm.h"

#define NFCT_FIELDPRESENT_FIELDPRESENT_Msk 1u
#define NFCT_FIELDPRESENT_LOCKDETECT_Msk 2u
typedef struct { uint32_t CYCCNT; } test_dwt_t;
typedef struct { uint32_t FIELDPRESENT; } test_nfct_t;
static test_dwt_t clock_state;
static test_nfct_t nfct;
static uint32_t falls[300], rises[300];
static unsigned pulses;
static uint32_t window_end, last_field_cycles, observed_eof_cycles;
static iso15693_frame_t frame;
static void *tick(void) { clock_state.CYCCNT += 8; return &clock_state; }
#define DWT ((test_dwt_t *)tick())
static void *field(void) {
    nfct.FIELDPRESENT = 3;
    for (unsigned i = 0; i < pulses; i++)
        if (clock_state.CYCCNT >= falls[i] && clock_state.CYCCNT < rises[i]) {
            nfct.FIELDPRESENT = 1;
            break;
        }
    return &nfct;
}
#define NRF_NFCT ((test_nfct_t *)field())
static bool nfcv_session_slot_pending(void) { return false; }

/* RECEIVER */

static void pulse(unsigned tick_index, int ppm) {
    uint64_t cycles = (uint64_t)tick_index * 60413 / 100;
    falls[pulses] = 100000u + cycles * (1000000 + ppm) / 1000000;
    rises[pulses] = falls[pulses] + 1208;
    pulses++;
}
static void exercise(bool slow, int ppm, bool valid_crc) {
    uint8_t request[12] = {0x22,0x2b,0x41,0x87,0xfb,0x19,0x50,0x03,0x04,0xe0};
    iso15693_append_crc(request, 10);
    if (!valid_crc) request[10] ^= 1;
    pulses = 0;
    pulse(0, ppm);
    pulse(slow ? 7 : 5, ppm);
    unsigned symbols = 0;
    for (unsigned i = 0; i < sizeof(request); i++) {
        if (slow) pulse(8 + symbols++ * 512 + request[i] * 2 + 1, ppm);
        else for (unsigned b = 0; b < 4; b++) {
            pulse(8 * (++symbols) + ((request[i] >> (b * 2)) & 3) * 2 + 1, ppm);
        }
    }
    pulse(slow ? 8 + symbols * 512 + 2 : 8 * (symbols + 1) + 2, ppm);
    clock_state.CYCCNT = 1;
    window_end = 1500000u * 64u;
    assert(receive_frame_or_slot() == valid_crc);
    if (valid_crc) {
        assert(frame.byte_count == sizeof(request));
        assert(!memcmp(frame.bytes, request, sizeof(request)));
        assert(frame.high_data_rate && frame.single_subcarrier);
        assert(observed_eof_cycles && last_field_cycles);
    }
}
int main(void) {
    for (int ppm = -75; ppm <= 75; ppm += 75)
        for (unsigned slow = 0; slow < 2; slow++) {
            exercise(slow, ppm, true);
            exercise(slow, ppm, false);
        }
    return 0;
}
