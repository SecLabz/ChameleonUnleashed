"""Shared HFXO demand, including Nordic DCD's skipped-acquisition path."""
from pathlib import Path
import re
import unittest
from native_test import run_c, function
ROOT = Path(__file__).resolve().parents[2]


class HfclkTests(unittest.TestCase):
    def test_owners_failures_and_usb_lifetime(self):
        source = (ROOT / 'firmware/application/src/utils/hfclk.c').read_text()
        source = re.sub(r'^#include.*$', '', source, flags=re.M)
        wrapper = (ROOT / 'firmware/application/src/usb_controller.c').read_text()
        helpers = wrapper[wrapper.index('static uint32_t usb_clock_request'):wrapper.index('#define sd_clock_hfclk_request')]
        power = function(wrapper, 'tusb_hal_nrf_power_event')
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define NRF_SUCCESS 0
#define NRF_ERROR_INVALID_PARAM 7
#define NRF_ERROR_TIMEOUT 13
#define APP_ERROR_CHECK(x) assert((x)==0)
enum {HFCLK_USB=1,HFCLK_LF=2,HFCLK_NFC=4,HFCLK_READER=8};
enum {NRF_DRV_POWER_USB_EVT_DETECTED,NRF_DRV_POWER_USB_EVT_REMOVED,NRF_DRV_POWER_USB_EVT_READY};
static bool critical,physical;
static uint32_t request_error,release_error,query_error;
static unsigned requests,releases,delays,forwarded;
static struct {unsigned ENABLE;} usbd;
#define NRF_USBD (&usbd)
uint32_t sd_nvic_critical_region_enter(uint8_t*n){assert(!critical);critical=true;*n=0;return 0;}
uint32_t sd_nvic_critical_region_exit(uint8_t n){assert(critical && !n);critical=false;return 0;}
uint32_t sd_clock_hfclk_request(void){assert(critical);requests++;if(!request_error)physical=true;return request_error;}
uint32_t sd_clock_hfclk_release(void){assert(critical);releases++;if(!release_error)physical=false;return release_error;}
uint32_t sd_clock_hfclk_is_running(uint32_t*r){assert(!critical);*r=physical;return query_error;}
void nrf_delay_us(unsigned us){assert(!critical && us==10);delays++;}
''' + source + helpers + r'''
void nordic_power_event(uint32_t e){
 forwarded++;
 if(e==0){usbd.ENABLE=1;if(!physical)(void)usb_clock_request();}
 if(e==1 && usbd.ENABLE){usbd.ENABLE=0;(void)usb_clock_release();}
}
''' + power + r'''
int main(void){
 for(unsigned first=1;first<=2;first++){
  unsigned second=first==1?2:1;requests=releases=0;
  assert(app_hfclk_acquire(first)==0 && owners==first && requests==1);
  assert(app_hfclk_acquire(first)==0 && requests==1);
  assert(app_hfclk_acquire(second)==0 && owners==3 && requests==1);
  assert(app_hfclk_release(first)==0 && owners==second && releases==0 && physical);
  assert(app_hfclk_release(first)==0 && releases==0);
  assert(app_hfclk_release(second)==0 && owners==0 && releases==1 && !physical);
 }
 for(unsigned first=1;first<=8;first*=2){
  for(unsigned second=1;second<=8;second*=2){
   requests=releases=0;
   assert(app_hfclk_acquire(first)==0);
   assert(app_hfclk_acquire(second)==0 && owners==(first|second) && requests==1);
   assert(app_hfclk_release(first)==0);
   if(first!=second) assert(owners==second && physical && releases==0);
   assert(app_hfclk_release(second)==0 && !owners && !physical && releases==1);
  }
 }
 assert(app_hfclk_acquire(3)==NRF_ERROR_INVALID_PARAM && owners==0);
 request_error=99;assert(app_hfclk_acquire(1)==99 && owners==0);request_error=0;
 assert(app_hfclk_acquire(1)==0);release_error=88;
 assert(app_hfclk_release(1)==88 && owners==1 && physical);release_error=0;
 assert(app_hfclk_release(1)==0 && owners==0);
 physical=true;requests=0;assert(app_hfclk_acquire(1)==0 && requests==1);
 assert(app_hfclk_wait()==0);assert(app_hfclk_release(1)==0);
 query_error=66;assert(app_hfclk_wait()==66);query_error=0;
 delays=0;assert(app_hfclk_wait()==NRF_ERROR_TIMEOUT && delays==1000);
 /* LF already started HFXO: DCD would skip acquisition without our wrapper. */
 assert(app_hfclk_acquire(2)==0);requests=releases=0;
 tusb_hal_nrf_power_event(0);assert(owners==3 && usbd.ENABLE && requests==0);
 tusb_hal_nrf_power_event(0);assert(owners==3 && requests==0);
 tusb_hal_nrf_power_event(1);assert(owners==2 && physical && releases==0);
 tusb_hal_nrf_power_event(1);assert(owners==2 && physical);
 unsigned before=forwarded;tusb_hal_nrf_power_event(2);assert(forwarded==before);
 assert(app_hfclk_release(2)==0 && releases==1);
 /* Opposite direction: reader transition releases LF while USB remains live. */
 tusb_hal_nrf_power_event(0);assert(app_hfclk_acquire(2)==0);
 assert(app_hfclk_release(2)==0 && owners==1 && physical && usbd.ENABLE);
 tusb_hal_nrf_power_event(1);assert(owners==0 && !physical);
 /* NFC oscillator must survive independent USB/LF teardown. */
 assert(app_hfclk_acquire(4)==0 && owners==4 && physical);
 tusb_hal_nrf_power_event(0);assert(owners==5 && usbd.ENABLE);
 assert(app_hfclk_acquire(2)==0 && owners==7);
 tusb_hal_nrf_power_event(1);assert(owners==6 && physical);
 assert(app_hfclk_release(2)==0 && owners==4 && physical);
 assert(app_hfclk_release(4)==0 && !owners && !physical);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
