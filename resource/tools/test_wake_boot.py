"""Use the SDK token once, preserve SHA validation and cancel returned OFFs."""
from pathlib import Path
import re
import unittest
from native_test import run_c

ROOT = Path(__file__).resolve().parents[2]


class WakeBootTests(unittest.TestCase):
    def test_token_ownership_errors_and_retained_marker_decoding(self):
        header = (ROOT / 'firmware/application/src/wake_boot.h').read_text()
        header = re.sub(r'^#(?:include|pragma).*$', '', header, flags=re.M)
        sdk = (ROOT / 'firmware/nrf52_sdk/components/libraries/bootloader/nrf_bootloader_info.h').read_text()
        constants = '\n'.join(line for line in sdk.splitlines()
                              if line.startswith('#define BOOTLOADER_DFU_')
                              and ('GPREGRET2' in line or 'SKIP_CRC' in line))
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define NRF_SUCCESS 0
static uint32_t regret;
static unsigned sets,clears,gets,get_error,set_error,clear_error;
unsigned sd_power_gpregret_get(unsigned n,uint32_t *v){assert(n==1);gets++;*v=regret;return get_error;}
unsigned sd_power_gpregret_set(unsigned n,uint32_t v){assert(n==1);sets++;if(!set_error)regret|=v;return set_error;}
unsigned sd_power_gpregret_clr(unsigned n,uint32_t v){assert(n==1);clears++;if(!clear_error)regret&=~v;return clear_error;}
''' + constants + '\n' + header + r'''
int main(void) {
 assert(BOOTLOADER_DFU_SKIP_CRC==0xa9 && BOOTLOADER_DFU_SKIP_CRC_MASK==0xf9);
 assert(wake_boot_cancel_off() && !gets && !clears); /* Cannot cancel an unowned token. */
  regret=2;assert(!wake_boot_prepare_off() && regret==2 && !sets);
  regret=0;get_error=1;assert(!wake_boot_prepare_off() && !sets);get_error=0;
  set_error=1;assert(!wake_boot_prepare_off() && regret==0);set_error=0;
  assert(wake_boot_prepare_off() && regret==0xa9);
  assert(!wake_boot_prepare_off() && regret==0xa9);
  get_error=1;assert(!wake_boot_cancel_off() && regret==0xa9);get_error=0;
  clear_error=1;assert(!wake_boot_cancel_off() && regret==0xa9);clear_error=0;
  regret|=2;assert(wake_boot_cancel_off() && regret==2);
  unsigned done=clears;assert(wake_boot_cancel_off() && clears==done);
 for(unsigned v=0;v<256;v++) {
  unsigned expected=(v&0xf9)==0xa9 ? v&~0xa9u : v;
  assert(wake_boot_app_regret(v)==expected);
 }
 assert(wake_boot_app_regret(1)==1 && wake_boot_app_regret(2)==2);
}
''')

    def test_stock_skip_only_bypasses_crc_for_both_images(self):
        source = (ROOT / 'firmware/nrf52_sdk/components/libraries/bootloader/nrf_bootloader.c').read_text()
        function = source[source.index('static bool boot_validate('):].split('\n}\n', 1)[0] + '\n}\n'
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define VALIDATE_CRC 1
typedef struct {unsigned type;} boot_validation_t;
static unsigned validated;
bool nrf_dfu_validation_boot_validate(const boot_validation_t *v,uint32_t addr,uint32_t length) {
 (void)v;assert(addr && length);validated++;return true;
}
''' + function + r'''
int main(void) {
 for(unsigned type=0;type<4;type++) {
  boot_validation_t v={type};validated=0;
  assert(boot_validate(&v,0x1000,0x26000,false));
  assert(validated==(type==1?0u:1u)); /* SD CRC can skip; SHA/signatures cannot. */
  validated=0;assert(boot_validate(&v,0x27000,460000,true));assert(validated==1);
 }
}
''')


if __name__ == '__main__':
    unittest.main()
