"""Host checks for USB identity, source routing and the embedded HTTP boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from native_test import function
from build_web import RESOURCE_ORIGIN_MAX_BYTES, resource_origin, source_version

ROOT=Path(__file__).resolve().parents[2]
NET=ROOT/'firmware/application/src/net'

class WebContracts(unittest.TestCase):
    def test_st25tb_http_wire_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'app_timer.h').write_text(
                '#include <stdint.h>\n#define APP_TIMER_TICKS(ms) ((ms)*16384u/1000u)\n'
                'uint32_t app_timer_cnt_get(void);\n'
                'uint32_t app_timer_cnt_diff_compute(uint32_t,uint32_t);\n')
            binary = directory / 'tearoff'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Os',
                '-fsanitize=address,undefined',
                '-I', str(directory), '-I', str(NET),
                '-I', str(ROOT / 'firmware/application/src'),
                '-I', str(ROOT / 'firmware/application/src/rfid/reader/hf'),
                str(ROOT / 'resource/tools/tests/web_tearoff.c'),
                str(NET / 'web_tearoff.c'), str(NET / 'bytes.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_uid_bound_read_only_type2_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            (directory/'rc522.h').write_text('#include <stdbool.h>\n#include <stdint.h>\ntypedef struct {uint8_t uid_len,uid[10],sak;} picc_14a_tag_t;\nvoid pcd_14a_reader_fast_halt_tag(void);\nuint8_t pcd_14a_reader_scan_auto(picc_14a_tag_t *);\nuint8_t pcd_14a_reader_raw_cmd(bool,bool,bool,bool,bool,bool,uint16_t,uint16_t,uint8_t *,uint8_t *,uint16_t *,uint16_t);\n')
            binary=directory/'type2'
            hf=ROOT/'firmware/application/src/rfid/reader/hf'
            # Compile the actual reader source with only RF I/O replaced.
            source=(hf/'type2_commands.c').read_text().replace('#include "rc522.h"','#include <rc522.h>')
            (directory/'type2.c').write_text(source)
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-fsanitize=address,undefined',
                '-I',str(directory),'-I',str(hf),'-I',str(ROOT/'firmware/application/src'),
                str(ROOT/'resource/tools/tests/web_type2.c'),str(directory/'type2.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_bounded_lf_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            (directory/'lf_reader_data.h').write_text('#include <stdbool.h>\n#include <stdint.h>\nbool em410x_read(uint8_t *,uint32_t);\nbool hidprox_read(uint8_t *,uint8_t,uint32_t);\nbool ioprox_read(uint8_t *,uint8_t,uint32_t);\nbool pac_read(uint8_t *,uint32_t);\nbool viking_read(uint8_t *,uint32_t);\nbool jablotron_read(uint8_t *,uint32_t);\n')
            (directory/'lf_tag_em.h').write_text('#include "tag_base_type.h"\n#define DEVICE_MODE_READER 1\nint get_device_mode(void);\n#define LF_EM410X_TAG_ID_SIZE 5\n#define LF_EM410X_ELECTRA_TAG_ID_SIZE 13\n#define LF_PAC_TAG_ID_SIZE 8\n#define LF_VIKING_TAG_ID_SIZE 4\n#define LF_JABLOTRON_TAG_ID_SIZE 5\n')
            (directory/'lf_t55xx.h').write_text('#include <stddef.h>\n#include <stdint.h>\nuint16_t lf_t55xx_command(const uint8_t *,size_t,uint8_t *,uint16_t *);\n')
            binary=directory/'lf'
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-fsanitize=address,undefined',
                '-I',str(directory),'-I',str(NET),'-I',str(ROOT/'firmware/application/src'),
                '-I',str(ROOT/'firmware/application/src/rfid/nfctag'),
                str(ROOT/'resource/tools/tests/web_lf.c'),str(NET/'lf_commands.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_slot_codecs_and_persistent_transactions(self):
        tags=ROOT/'firmware/application/src/rfid/nfctag'
        source=(tags/'tag_emulation.c').read_text()
        storage='\n'.join(function(source,name) for name in [
            'tag_emulation_sense_switch','tag_emulation_sense_switch_all',
            'tag_emulation_sense_end','tag_emulation_sense_run','tag_emulation_sense_hold',
            'tag_emulation_load_by_buffer','load_data_by_tag_type',
            'save_data_by_tag_type','sync_snapshot_buffer','tag_emulation_place_revision','store_slot_config','tag_emulation_snapshot','restore_slot_buffer',
            'tag_emulation_slot_configure','tag_emulation_select_checked'])
        type2=(tags/'hf/nfc_mf0_ntag.c').read_text()
        geometry='\n'.join(function(type2,name) for name in [
            'nfc_tag_mf0_ntag_get_nr_pages_by_tag_type','get_total_pages_by_tag_type',
            'get_information_size_by_tag_type','nfc_tag_mf0_ntag_image_size'])
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            (directory/'app_util.h').write_text('#define STATIC_ASSERT(c) _Static_assert(c,"layout")\n')
            (directory/'rfid_main.h').write_text('''#pragma once
#include "tag_emulation.h"
#define DEVICE_MODE_READER 1
#define DEVICE_MODE_TAG 2
int get_device_mode(void);
void tag_mode_enter(void);
void apply_slot_change(uint8_t,uint8_t);
void light_up_by_slot(void);
''')
            (directory/'fds_util.h').write_text('''#include <stdint.h>
#include <stdbool.h>
bool fds_read_sync(uint16_t,uint16_t,uint16_t *,uint8_t *);
bool fds_write_sync(uint16_t,uint16_t,uint16_t,void *);
int fds_delete_sync(uint16_t,uint16_t);
bool fds_is_exists(uint16_t,uint16_t);
uint32_t fds_hash_sync(uint16_t,uint16_t,uint32_t);
uint32_t fds_hash_bytes(uint32_t,const uint8_t *,size_t);
''')
            fixture=(ROOT/'resource/tools/tests/web_slots.c').read_text()
            fixture=fixture.replace('/* PRODUCTION_HASH */',function((ROOT/'firmware/application/src/utils/fds_util.c').read_text(),'fds_hash_bytes'))
            fixture=fixture.replace('/* PRODUCTION_ST25_SAVE */',function((tags/'hf/nfc_st25tb.c').read_text(),'nfc_st25tb_data_savecb'))
            fixture=fixture.replace('/* PRODUCTION_GEOMETRY */',geometry).replace('/* PRODUCTION_STORAGE */',storage)
            path = directory / 'slots.c'
            path.write_text(fixture)
            binary = directory / 'slots'
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-Wno-unused-parameter',
                '-fshort-enums','-fsanitize=address,undefined',
                '-I',str(directory),'-I',str(NET),'-I',str(tags),'-I',str(tags/'hf'),'-I',str(tags/'lf'),
                '-I',str(ROOT/'firmware/common'),'-I',str(ROOT/'firmware/application/src'),
                '-I',str(ROOT/'firmware/application/src/utils'),str(path),str(NET/'slot_api.c'),
                str(NET/'slot_image.c'),str(ROOT/'firmware/application/src/rfid/nfctag/hf/desfire_model.c'),str(ROOT/'firmware/application/src/rfid/st25tb/model.c'),
                *(str(ROOT/'firmware/application/src/rfid/iso15693'/f'{name}.c') for name in ('frame','memory','standard','slix','session')),
                '-I',str(ROOT/'firmware/application/src/rfid'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_fds_read_closes_success_and_short_buffer_records(self):
        source=(ROOT/'firmware/application/src/utils/fds_util.c').read_text()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'fds.c'
            binary = Path(tmp) / 'fds'
            fixture=(ROOT/'resource/tools/tests/fds_read.c').read_text()
            path.write_text(fixture.replace('/* PRODUCTION_READ */','\n'.join(function(source,name) for name in ['fds_read_sync','fds_hash_bytes','fds_hash_sync'])))
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-Wno-unused-parameter',
                '-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_device_metadata_and_persistent_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            (directory/'rfid_main.h').write_text('''#include <stdbool.h>
#include <stdint.h>
#include "tag_base_type.h"
#define DEVICE_MODE_READER 1
typedef struct { uint32_t DEVICEID[2]; } test_ficr_t;
extern test_ficr_t test_ficr;
#define NRF_FICR (&test_ficr)
int get_device_mode(void);
uint8_t tag_emulation_get_slot(void);
uint32_t tag_emulation_place_revision(uint8_t,tag_sense_type_t);
void tag_emulation_get_specific_types_by_slot(uint8_t,tag_slot_specific_type_t *);
bool is_slot_enabled(uint8_t,tag_sense_type_t);
''')
            (directory/'ble_main.h').write_text('#include <stdint.h>\nextern uint16_t batt_lvl_in_milli_volts;\nextern uint8_t percentage_batt_lvl;\n')
            (directory/'fds_util.h').write_text('#include <stdbool.h>\n#include <stdint.h>\nbool fds_read_sync(uint16_t,uint16_t,uint16_t *,uint8_t *);\n')
            (directory/'tag_persistence.h').write_text('typedef struct {uint16_t key,id;} fds_slot_record_map_t;\nvoid get_fds_map_by_slot_sense_type_for_nick(uint8_t,tag_sense_type_t,void *);\n')
            settings=(ROOT/'firmware/application/src/settings.h').read_text()
            (directory/'settings.h').write_text(settings.replace('#include "utils.h"','#define ALIGN_U32 __attribute__((aligned(4)))'))
            binary=directory/'device'
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',
                '-DAPP_FW_VER_MAJOR=2','-DAPP_FW_VER_MINOR=2','-DGIT_VERSION="host-test"',
                '-I',str(directory),'-I',str(NET),'-I',str(ROOT/'firmware/application/src'),
                '-I',str(ROOT/'firmware/application/src/rfid/nfctag'),
                str(ROOT/'resource/tools/tests/web_device.c'),str(NET/'device_api.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_usb_reset_close_and_ble_backpressure(self):
        usb=(ROOT/'firmware/application/src/usb_main.c').read_text()
        ble=(ROOT/'firmware/application/src/ble_main.c').read_text()
        callbacks='\n'.join(function(usb,name) for name in [
            'connection_end','reset_transport','tud_event_hook_cb','tud_mount_cb',
            'tud_cdc_line_state_cb','tud_umount_cb','usb_cdc_write','usb_cdc_tx_ready',
            'usb_cdc_tearoff_end','usb_poll'])
        callbacks+='\n'+function(ble,'nus_data_response')
        callbacks+='\n'+function((ROOT/'firmware/application/src/app_cmd.c').read_text(),'auto_response_data')
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'lifecycle.c'
            binary = Path(tmp) / 'lifecycle'
            fixture=(ROOT/'resource/tools/tests/transport_lifecycle.c').read_text()
            source.write_text(fixture.replace('/* PRODUCTION_CALLBACKS */',callbacks))
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-fsanitize=address,undefined',
                '-I',str(ROOT/'firmware/application/src'),str(source),
                str(ROOT/'firmware/application/src/usb_tearoff.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_fragmented_transports_and_disconnect(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            (directory/'nrf_log.h').write_text('#define NRF_LOG_MODULE_REGISTER()\n#define NRF_LOG_ERROR(...) ((void)0)\n#define NRF_LOG_INFO(...) ((void)0)\n')
            for name in ['nrf_log_ctrl.h', 'nrf_log_default_backends.h']:
                (directory / name).write_text('')
            binary=directory/'test'
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-fsanitize=address,undefined',
                '-I',str(directory),'-I',str(ROOT/'firmware/application/src'),'-I',str(ROOT/'firmware/common'),'-I',str(ROOT/'firmware/application/src/utils'),
                str(ROOT/'resource/tools/tests/dataframe.c'),str(ROOT/'firmware/application/src/utils/dataframe.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_usb_serial_network_identity_and_rndis(self):
        tiny=ROOT/'.cache/web/deps/tinyusb'
        if not tiny.exists():
            self.skipTest('Prepare pinned TinyUSB dependencies first')
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'usb'
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-DPLATFORM_HOST','-DLWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS','-fsanitize=address,undefined',
                '-I',str(NET),'-I',str(tiny/'src'),'-I',str(tiny/'lib/networking'),
                '-I',str(ROOT/'.cache/web/deps/lwip/src/include'),
                str(ROOT/'resource/tools/tests/usb_network.c'),str(ROOT/'firmware/application/src/usb_main.c'),str(NET/'rndis.c'),str(NET/'bytes.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_pending_tcp_output_and_disconnect(self):
        lwip = ROOT / '.cache/web/deps/lwip/src'
        if not lwip.is_dir():
            self.skipTest('Prepare pinned lwIP dependencies first')
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'app_timer.h').write_text(
                '#include <stdint.h>\n#define APP_TIMER_CONFIG_RTC_FREQUENCY 0\n'
                '#define APP_TIMER_CLOCK_FREQ 32768\n'
                'uint32_t app_timer_cnt_get(void);\n'
                'uint32_t app_timer_cnt_diff_compute(uint32_t,uint32_t);\n')
            (directory / 'tusb.h').write_text(
                '#include <stdbool.h>\n#include <stdint.h>\n'
                'bool tud_ready(void);\nbool tud_network_can_xmit(uint16_t);\n'
                'void tud_network_xmit(void *,uint16_t);\n'
                'void tud_network_recv_renew(void);\n')
            # Match host pointer alignment without changing the firmware memory budget.
            (directory / 'lwipopts.h').write_text((NET / 'lwipopts.h').read_text().replace(
                '#define MEM_ALIGNMENT 4', '#define MEM_ALIGNMENT 8'))
            binary = directory / 'network'
            sources = sorted((lwip / 'core').glob('*.c'))
            sources += sorted((lwip / 'core/ipv4').glob('*.c'))
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-DLWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS',
                '-fsanitize=address,undefined',
                '-I', str(directory), '-I', str(NET),
                '-I', str(ROOT / 'firmware/application/src'), '-I', str(lwip / 'include'),
                str(ROOT / 'resource/tools/tests/network_progress.c'),
                *map(str, sources), str(lwip / 'netif/ethernet.c'), '-o', str(binary),
            ], check=True)
            for scenario in ('empty_rx', 'syn', 'data', 'fin', 'ack', 'parallel', 'disconnect',
                             'responses', 'asset', 'nested', 'response_reconnect', 'late_data'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=5)

    def test_http_security_campaign_expiry_and_stale_requests(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            (directory/'app_timer.h').write_text('#include <stdint.h>\n#define APP_TIMER_TICKS(ms) ((ms)*16384u/1000u)\nuint32_t app_timer_cnt_get(void);\nuint32_t app_timer_cnt_diff_compute(uint32_t,uint32_t);\n')
            (directory/'rfid_main.h').write_text('extern bool g_is_tag_emulating;\ntypedef int device_mode_t;\n#define DEVICE_MODE_READER 1\nint get_device_mode(void);\nvoid reader_mode_enter(void);\nvoid tag_mode_enter(void);\n')
            (directory/'ble_main.h').write_text('#include <stdbool.h>\nbool ble_slave_rf_only(void);\n')
            origin = resource_origin('https://' + 'a' * (RESOURCE_ORIGIN_MAX_BYTES - 20) + '.cdn.example/')
            (directory/'web_assets.h').write_text(
                '#include <string.h>\n#define WEB_RESOURCE_CONNECT "\'self\' ' + origin + '"\n'
                'static const unsigned char page[]={1,2};\ntypedef struct { const char *type; const unsigned char *bytes; size_t size; } web_asset_t;\nstatic const web_asset_t *web_asset_find(const char *uri){static const web_asset_t page_asset={"text/html; charset=utf-8",page,2};return !strcmp(uri,"/")?&page_asset:NULL;}\n')
            binary=directory/'api'
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',
                '-DEXPECTED_RESOURCE_ORIGIN="' + origin + '"',
                '-DGIT_VERSION="host-test"','-I',str(directory),'-I',str(NET),
                '-I',str(ROOT/'firmware/application/src'),'-I',str(ROOT/'firmware/application/src/rfid/reader/hf'),
                '-I',str(ROOT/'firmware/application/src/rfid'),
                str(ROOT/'resource/tools/tests/web_api.c'),str(NET/'api.c'),str(NET/'http.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


class WebBuildTests(unittest.TestCase):
    def test_resource_base_derives_only_the_csp_origin(self):
        for base, expected in [
            ('https://cdn.example/assets/', 'https://cdn.example'),
            ('https://alice.github.io/ChameleonUnleashed/', 'https://alice.github.io'),
            ('http://localhost:8080/chameleon/', 'http://localhost:8080'),
            ('/chameleon/', ''),
            ('/', ''),
        ]:
            with self.subTest(base=base):
                self.assertEqual(resource_origin(base), expected)

    def test_resource_base_rejects_ambiguous_urls_and_csp_injection(self):
        for base in [
            'https://cdn.example/assets', '//cdn.example/assets/', 'assets/',
            'ftp://cdn.example/assets/', 'https://user:secret@cdn.example/',
            'https://cdn.example/?query/', 'https://cdn.example/#fragment/',
            "https://cdn.example/;connect-src 'none'/", 'https://cdn.example/\n/',
            'https://cdn.example:invalid/', 'https://cdn.example:0/',
        ]:
            with self.subTest(base=base):
                with self.assertRaises(ValueError):
                    resource_origin(base)

    def test_resource_origin_respects_the_firmware_header_budget(self):
        origin = 'https://' + 'a' * (RESOURCE_ORIGIN_MAX_BYTES - 20) + '.cdn.example'
        self.assertEqual(len(origin.encode('utf-8')), RESOURCE_ORIGIN_MAX_BYTES)
        self.assertEqual(resource_origin(origin + '/assets/'), origin)
        with self.assertRaisesRegex(ValueError, 'firmware HTTP headers'):
            resource_origin(origin + 'a/')

    def test_source_provenance_distinguishes_clean_local_and_unversioned_builds(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.assertEqual(source_version(directory), {'revision': None, 'dirty': None})
            subprocess.run(['git', 'init', '-q', str(directory)], check=True)
            subprocess.run([
                'git', '-C', str(directory), '-c', 'user.name=Test',
                '-c', 'user.email=test@example.invalid', '-c', 'commit.gpgsign=false',
                'commit', '-qm', 'Initial snapshot', '--allow-empty',
            ], check=True)
            revision = subprocess.check_output(
                ['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
            self.assertEqual(source_version(directory), {'revision': revision, 'dirty': False})
            (directory / 'local.txt').write_text('edited sources')
            self.assertEqual(source_version(directory), {'revision': revision, 'dirty': True})


if __name__ == '__main__':
    unittest.main()
