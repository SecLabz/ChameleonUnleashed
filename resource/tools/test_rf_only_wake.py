"""Exercise production RF admission, security exclusion and control handoff."""
from pathlib import Path
import unittest
from native_test import run_c, function as extract_function

ROOT = Path(__file__).resolve().parents[2]


class RfOnlyWakeTests(unittest.TestCase):
    def test_battery_protection_without_ble_services(self):
        source = (ROOT / 'firmware/application/src/ble_main.c').read_text()
        function = source[source.index('static void battery_level_meas_timeout_handler'):].split('\n}\n', 1)[0] + '\n}\n'
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define UNUSED_PARAMETER(x) (void)(x)
#define APP_ERROR_CHECK(x) assert((x)==0)
#define APP_ERROR_HANDLER(x) assert(0)
#define NRF_LOG_INFO(...) ((void)0)
#define ADC_RESULT_IN_MILLI_VOLTS(x) (x)
#define BATVOL2PERCENT(x) ((x)>3200?50:0)
enum { NRF_SUCCESS, NRF_ERROR_INVALID_STATE, NRF_ERROR_RESOURCES, NRF_ERROR_BUSY,
 NRF_ERROR_FORBIDDEN, BLE_ERROR_GATTS_SYS_ATTR_MISSING, ADC_CHANNEL, BLE_CONN_HANDLE_ALL,
 SLEEP_NO_BATTERY_SHUTDOWN };
typedef unsigned ret_code_t;
typedef int nrf_saadc_value_t;
static bool rf_only_boot, g_is_low_battery_shutdown, services;
static void *m_lf_adc_callback;
static unsigned batt_lvl_in_milli_volts, percentage_batt_lvl, measured, updated, sleeps;
static int m_bas, sample=3600;
unsigned nrfx_saadc_sample_convert(unsigned channel,int *value){assert(channel==ADC_CHANNEL);*value=sample;measured++;return 0;}
unsigned ble_bas_battery_level_update(int *service,unsigned percent,unsigned conn){
 assert(services && !rf_only_boot && service==&m_bas && percent==50 && conn==BLE_CONN_HANDLE_ALL);
 updated++;return 0;
}
void sleep_timer_start(unsigned delay){assert(delay==SLEEP_NO_BATTERY_SHUTDOWN);sleeps++;}
''' + function + r'''
int main(void) {
 services=true;battery_level_meas_timeout_handler(NULL);assert(measured==1 && updated==1 && !sleeps);
 rf_only_boot=true;services=false;battery_level_meas_timeout_handler(NULL);
 assert(measured==2 && updated==1 && batt_lvl_in_milli_volts==3700 && !g_is_low_battery_shutdown);
 sample=1000;battery_level_meas_timeout_handler(NULL);
 assert(measured==3 && updated==1 && g_is_low_battery_shutdown && sleeps==1);
 m_lf_adc_callback=&m_bas;battery_level_meas_timeout_handler(NULL);assert(measured==3);
}
''')

    def test_fresh_seed_precedes_softdevice_only_for_admitted_rf_boots(self):
        source = (ROOT / 'firmware/application/src/app_main.c').read_text()
        function = source[source.index('static bool rng_seed_before_ble'):].split('\n}\n', 1)[0] + '\n}\n'
        main = source[source.index('int main(void)'):]
        sequence = main[main.index('bool rng_seeded ='):main.index('bsp_timer_init();')]
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define POWER_RESETREAS_NFC_Msk (1u<<19)
#define HFCLK_NFC 4
#define APP_ERROR_CHECK(x) assert((x)==0)
static struct {uint32_t RESETREAS;} power;
#define NRF_POWER (&power)
static bool safe,sd_enabled,early;
static unsigned seeds,security;
bool tag_emulation_rf_only_safe(void){return safe;}
void rng_drv_and_srand_init(void){assert(sd_enabled != early);seeds++;}
unsigned app_hfclk_acquire(unsigned owner){assert(sd_enabled && owner==4);return 0;}
unsigned app_hfclk_wait(void){assert(sd_enabled);return 0;}
void ble_slave_init(void){
 assert(seeds==(early?1u:0u));sd_enabled=true;
 if(!early)security++; /* Full boots retain security/TRNG before RNG. */
}
''' + function + r'''
int main(void) {
 uint32_t reasons[]={0,4,1u<<16,1u<<19,(1u<<19)|4,1u<<20};
 for(unsigned i=0;i<6;i++) for(unsigned eligible=0;eligible<2;eligible++) {
  power.RESETREAS=reasons[i];safe=eligible;sd_enabled=false;seeds=security=0;
  early=safe && reasons[i]==POWER_RESETREAS_NFC_Msk;
''' + sequence + r'''
  assert(sd_enabled && seeds==1 && security==(early?0u:1u));
 }
}
''')

    def test_usb_waits_for_both_protocol_and_physical_field_loss(self):
        source = (ROOT / 'firmware/application/src/app_main.c').read_text()
        function = source[source.index('static void usb_start_when_ready'):].split('\n}\n', 1)[0] + '\n}\n'
        run_c(r'''
#include <stdbool.h>
#include <assert.h>
static bool m_usb_started,rf_only,g_is_tag_emulating,field;
static unsigned starts;
bool ble_slave_rf_only(void){return rf_only;}
bool nrfx_nfct_field_check(void){return field;}
void usb_start(void){starts++;}
''' + function + r'''
int main(void) {
 rf_only=true;field=true;usb_start_when_ready();assert(!starts && !m_usb_started);
 field=false;g_is_tag_emulating=true;usb_start_when_ready();assert(!starts);
 g_is_tag_emulating=false;usb_start_when_ready();assert(starts==1 && m_usb_started);
 field=true;usb_start_when_ready();assert(starts==1);
 m_usb_started=false;rf_only=false;usb_start_when_ready();assert(starts==2 && m_usb_started);
}
''')

    def test_profile_gate_rejects_crypto_profiles_and_bad_records(self):
        source = (ROOT / 'firmware/application/src/rfid/nfctag/tag_emulation.c').read_text()
        function = extract_function(source, 'tag_emulation_rf_only_safe')
        run_c(r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define FDS_EMULATION_CONFIG_FILE_ID 4096
#define FDS_EMULATION_CONFIG_RECORD_KEY 1
#define TAG_SLOT_CONFIG_CURRENT_VERSION 8
#define TAG_MAX_SLOT_NUM 8
#define TAG_TYPE_MIFARE_Mini 1000
#define TAG_TYPE_MIFARE_4096 1003
#define TAG_TYPE_NTAG_213 1100
#define TAG_TYPE_MF0ICU2 1104
#define TAG_TYPE_NTAG_212 1108
#define TAG_TYPE_DESFIRE 1300
#define TAG_TYPE_SLIX_L 1500
#define TAG_TYPE_SLIX 1501
#define TAG_TYPE_ST25TB512_AT 2000
#define TAG_TYPE_EM410X 100
#define TAG_TYPE_UNDEFINED 0
typedef unsigned tag_specific_type_t;
struct slot { bool enabled_hf; unsigned tag_hf, tag_lf; };
typedef struct {unsigned version, active_slot; struct slot slots[8];} tag_slot_config_t;
static tag_slot_config_t saved;
static bool readable = true;
static uint16_t actual = sizeof(saved);
bool fds_read_sync(unsigned f,unsigned k,uint16_t *length,uint8_t *buffer) {
 assert(f==4096 && k==1 && *length==sizeof(saved));
 memcpy(buffer,&saved,sizeof(saved));*length=actual;return readable;
}
''' + function + r'''
int main(void) {
 saved.version = 8;
 saved.slots[0].enabled_hf = true;
 unsigned admitted[] = {TAG_TYPE_MIFARE_Mini, TAG_TYPE_MIFARE_4096,
     TAG_TYPE_NTAG_213, TAG_TYPE_NTAG_212, TAG_TYPE_DESFIRE,
     TAG_TYPE_SLIX_L, TAG_TYPE_SLIX, TAG_TYPE_ST25TB512_AT};
 for (unsigned i = 0; i < sizeof(admitted) / sizeof(admitted[0]); i++) {
  saved.slots[0].tag_hf = admitted[i];
  assert(tag_emulation_rf_only_safe());
 }

 unsigned rejected[] = {0, 999, 1004, 1099, TAG_TYPE_MF0ICU2, 1109, 1301, 2001};
 for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
  saved.slots[0].tag_hf = rejected[i];
  assert(!tag_emulation_rf_only_safe());
 }

 saved.slots[0].tag_hf = 1001;
 saved.slots[0].tag_lf = TAG_TYPE_EM410X;
 assert(tag_emulation_rf_only_safe());
 saved.slots[0].tag_lf = 101;
 assert(!tag_emulation_rf_only_safe());
 saved.slots[0].tag_lf = 0;
 saved.slots[0].enabled_hf = false;
 assert(!tag_emulation_rf_only_safe());
 saved.slots[0].enabled_hf = true;
 saved.slots[7] = saved.slots[0];
 saved.active_slot = 7;
 assert(tag_emulation_rf_only_safe());
 saved.active_slot=8;assert(!tag_emulation_rf_only_safe());saved.active_slot=0;
 saved.version=7;assert(!tag_emulation_rf_only_safe());saved.version=8;
 actual--;assert(!tag_emulation_rf_only_safe());actual++;
 readable=false;assert(!tag_emulation_rf_only_safe());
}
''')

    def test_rf_boot_skips_ble_services_and_full_boot_initializes_them(self):
        source = (ROOT / 'firmware/application/src/ble_main.c').read_text()
        functions = source[source.index('void ble_slave_init(void)'):source.index('void register_lf_adc_callback')]
        run_c(r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define POWER_RESETREAS_NFC_Msk (1u<<19)
#define APP_ERROR_CHECK(x) assert((x)==0)
static struct {uint32_t RESETREAS;} power;
#define NRF_POWER (&power)
static bool rf_only_boot,safe;
static unsigned security,crypto,steps,sd_only;
bool tag_emulation_rf_only_safe(void){return safe;}
uint32_t nrf_crypto_init(void){crypto++;return 0;}
uint32_t nrf_sdh_enable_request(void){assert(rf_only_boot);sd_only++;steps++;return 0;}
#define INIT(name) void name(void){steps++;}
INIT(adc_configure) INIT(create_battery_timer) INIT(ble_stack_init) INIT(gap_params_init)
INIT(gatt_init) INIT(services_init) INIT(advertising_init) INIT(conn_params_init)
void peer_manager_init(void){security++;}
''' + functions + r'''
int main(void) {
 uint32_t reasons[]={0,4,1u<<16,1u<<19,(1u<<19)|4,1u<<20};
 for(unsigned i=0;i<6;i++) for(unsigned eligible=0;eligible<2;eligible++) {
  power.RESETREAS=reasons[i];safe=eligible;
  rf_only_boot=false;security=crypto=steps=sd_only=0;
  bool fast=safe && reasons[i]==(1u<<19);
  ble_slave_init();assert(steps==(fast?3u:8u) && ble_slave_rf_only()==fast);
  assert(sd_only==(fast?1u:0u));
  assert(crypto==0 && security==(fast?0u:1u));
 }
}
''')

    def test_control_allowlist_excludes_mutation_and_crypto(self):
        source = (ROOT / 'firmware/application/src/app_cmd.c').read_text()
        function = extract_function(source, 'rf_only_command_allowed')
        run_c('#include <stdbool.h>\n#include <stdint.h>\n#include <assert.h>\n'
              f'#include "{ROOT}/firmware/application/src/data_cmd.h"\n' + function + r'''
int main(void) {
 assert(rf_only_command_allowed(DATA_CMD_ENTER_BOOTLOADER));
 assert(rf_only_command_allowed(DATA_CMD_GET_DEVICE_CHIP_ID));
 assert(rf_only_command_allowed(DATA_CMD_MF1_READ_EMU_BLOCK_DATA));
 assert(!rf_only_command_allowed(DATA_CMD_SET_ACTIVE_SLOT));
 assert(!rf_only_command_allowed(DATA_CMD_SET_SLOT_TAG_TYPE));
 assert(!rf_only_command_allowed(DATA_CMD_CHANGE_DEVICE_MODE));
 assert(!rf_only_command_allowed(0xffff));
}
''')

    def test_saved_pairing_only_configures_an_enabled_ble_stack(self):
        main = (ROOT / 'firmware/application/src/app_main.c').read_text()
        ble = (ROOT / 'firmware/application/src/ble_main.c').read_text()
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define BLE_PAIRING_KEY_LEN 6
#define BLE_GAP_OPT_PASSKEY 1
#define APP_ERROR_CHECK(x) assert((x)==0)
static bool rf_only_boot, pairing;
static unsigned sets;
static uint8_t saved_key[6]={'1','2','3','4','5','6'};
static struct {struct {struct {uint8_t *p_passkey;} passkey;} gap_opt;} m_static_pin_option;
bool settings_get_ble_pairing_enable_first_load(void){return pairing;}
uint8_t *settings_get_ble_connect_key(void){return saved_key;}
unsigned sd_ble_opt_set(unsigned option, const void *value){
 assert(!rf_only_boot && option==1 && value==&m_static_pin_option);
 assert(!memcmp(m_static_pin_option.gap_opt.passkey.p_passkey,saved_key,6));sets++;return 0;
}
''' + extract_function(ble, 'set_ble_connect_key') + '\n' +
              extract_function(main, 'ble_passkey_init') + r'''
int main(void){
 pairing=true;rf_only_boot=true;ble_passkey_init();assert(!sets);
 set_ble_connect_key(saved_key);assert(!sets);
 rf_only_boot=false;ble_passkey_init();assert(sets==1);
 pairing=false;ble_passkey_init();assert(sets==1);
}
''')


if __name__ == '__main__':
    unittest.main()
