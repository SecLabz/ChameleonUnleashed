"""Check that the RC selection is restricted to NFC wakes."""
from pathlib import Path
import re
import unittest
from native_test import run_c

ROOT = Path(__file__).resolve().parents[2]


class BleClockTests(unittest.TestCase):
    def test_nfc_only_and_return_code(self):
        source = (ROOT / 'firmware/application/src/ble_clock.c').read_text()
        source = source.replace('#include "nrf_sdh.c"', '''
uint32_t sdk_enable(nrf_clock_lf_cfg_t const *cfg, nrf_fault_handler_t handler) {
    return sd_softdevice_enable(cfg, handler);
}
''')
        source = re.sub(r'^#include.*$', '', source, flags=re.M)
        run_c(r'''
#include <stdint.h>
#include <assert.h>
typedef struct { uint8_t source, rc_ctiv, rc_temp_ctiv, accuracy; } nrf_clock_lf_cfg_t;
typedef void (*nrf_fault_handler_t)(uint32_t, uint32_t, uint32_t);
static struct {uint32_t RESETREAS;} power;
#define NRF_POWER (&power)
#define POWER_RESETREAS_NFC_Msk (1u<<19)
#define NRF_CLOCK_LF_SRC_RC 0
#define NRF_CLOCK_LF_ACCURACY_500_PPM 1
static nrf_clock_lf_cfg_t seen;
static uint32_t svc_result=1234;
static void fault(uint32_t a,uint32_t b,uint32_t c) {(void)a;(void)b;(void)c;}
static uint32_t sd_softdevice_enable(nrf_clock_lf_cfg_t const *cfg,nrf_fault_handler_t h) {
    assert(h==fault); seen=*cfg; return svc_result;
}
''' + source + r'''
int main(void) {
 const nrf_clock_lf_cfg_t xtal={1,0,0,7};
 uint32_t reasons[]={0,2,4,1u<<16,1u<<20,1u<<19,(1u<<19)|4};
 for(unsigned i=0;i<sizeof(reasons)/sizeof(reasons[0]);i++) {
    power.RESETREAS=reasons[i];
    assert(sdk_enable(&xtal,fault)==1234);
    if(reasons[i] == (1u<<19)) {
       assert(seen.source==0 && seen.rc_ctiv==16 && seen.rc_temp_ctiv==2 && seen.accuracy==1);
    } else assert(seen.source==1 && seen.rc_ctiv==0 && seen.rc_temp_ctiv==0 && seen.accuracy==7);
    assert(xtal.source==1 && xtal.rc_ctiv==0 && xtal.rc_temp_ctiv==0 && xtal.accuracy==7);
 }
 svc_result=0;power.RESETREAS=1u<<19;assert(sdk_enable(&xtal,fault)==0);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
