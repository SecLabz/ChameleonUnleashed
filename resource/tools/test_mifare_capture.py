"""Compile the changed capture functions with deterministic radio responses."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from native_test import function, run_c

ROOT = Path(__file__).resolve().parents[2]
HF = ROOT / 'firmware/application/src/rfid/reader/hf'


def run_fixture(fixture):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / 'test.c').write_text(fixture)
        (path / 'nrf.h').write_text('''#include <stdint.h>
typedef struct { uint32_t CTRL, CYCCNT; } test_dwt_t;
typedef struct { uint32_t DEMCR; } test_debug_t;
extern test_dwt_t test_dwt;
extern test_debug_t test_debug;
extern uint32_t SystemCoreClock;
#define DWT (&test_dwt)
#define CoreDebug (&test_debug)
#define CoreDebug_DEMCR_TRCENA_Msk 1
#define DWT_CTRL_CYCCNTENA_Msk 1
''')
        rfid = ROOT / 'firmware/application/src/rfid'
        subprocess.run([
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-fsanitize=address,undefined',
            '-I', str(path), '-I', str(HF), '-I', str(rfid), '-I', str(ROOT / 'firmware/application/src/bsp'), '-I', str(ROOT / 'firmware/application/src'),
            str(path / 'test.c'), str(rfid / 'hex_utils.c'),
            str(rfid / 'parity.c'), str(HF / 'mf1_capture_stats.c'), '-o', str(path / 'test'),
        ], check=True)
        subprocess.run([str(path / 'test')], check=True, timeout=10)

def capture_wrapper():
    return """
static data_frame_tx_t *cmd_processor_mf1_collect_observations(uint16_t cmd,
    uint16_t status, uint16_t length, uint8_t *data) {
    uint8_t output[272]; uint16_t count;
    status = mf1_collect_command(length, data, output, &count);
    return data_frame_make(cmd, status, count, output);
}
"""

class CaptureContract(unittest.TestCase):
    def test_dictionary_retries_silent_keys_and_stops_on_lost_card(self):
        source = function((HF / 'mf1_toolbox.c').read_text(), 'mf1_toolbox_check_keys_on_block')
        fixture = (Path(__file__).parent / 'tests/mifare_dictionary.c').read_text()
        run_fixture(fixture.replace('/* DICTIONARY FUNCTION */', source))

    def test_bounded_and_legacy_darkside_capture(self):
        source = (HF / 'mf1_toolbox.c').read_text()
        start = source.index('/* Legacy serial and bounded')
        end = source.index('/**\n* @brief    : Modify the middle delay', start)
        functions = '\n'.join(function(source, name) for name in [
            'darkside_reset_field', 'darkside_select_nonces']) + '\n' + source[start:end]
        fixture = (Path(__file__).parent / 'tests/mifare_darkside.c').read_text()
        run_fixture(fixture.replace('/* DARKSIDE FUNCTIONS */', functions))

    def test_reader_acquisition_clears_stale_spi_completion(self):
        source = function((HF / 'rc522.c').read_text(), 'pcd_14a_reader_init')
        fixture = (Path(__file__).parent / 'tests/mifare_reader_init.c').read_text()
        run_fixture(fixture.replace('/* READER INIT FUNCTION */', source))

    def test_reader_reset_checks_register_defaults_and_keeps_field_off_delay(self):
        source = function((HF / 'rc522.c').read_text(), 'pcd_14a_reader_reset')
        fixture = (Path(__file__).parent / 'tests/rc522_reset.c').read_text()
        run_c(fixture.replace('/* READER RESET FUNCTION */', source))

    def test_sector_read_geometry_permissions_and_single_authentication(self):
        source = function((HF / 'nfca_commands.c').read_text(), 'read_sector')
        fixture = (Path(__file__).parent / 'tests/mifare_sector.c').read_text()
        run_fixture(fixture.replace('/* SECTOR FUNCTION */', source))

    def test_type2_restore_identification_authentication_and_one_shot_writes(self):
        source = (HF / 'nfca_commands.c').read_text()
        functions = '\n'.join(function(source, name) for name in [
            'classic_model_check', 'type2_model_check', 'type2_password_auth', 'type2_auth_read',
            'write_user_unit',
        ])
        fixture = (Path(__file__).parent / 'tests/type2_restore.c').read_text()
        run_fixture(fixture.replace('/* PRODUCTION FUNCTIONS */', functions))

    def test_capture_and_legacy_wire_contracts(self):
        source = (HF / 'mf1_toolbox.c').read_text()
        header = (HF / 'mf1_toolbox.h').read_text()
        declarations = []
        for name in ['mf1_nested_core_t', 'mf1_observation_t']:
            text = source if name == 'mf1_observation_t' else header
            declarations.append(re.search(
                r'typedef struct\s*\{[^}]*\}\s*' + name + ';', text).group())
        for name in ['AUTH_FIRST', 'AUTH_NESTED']:
            declarations.append(re.search(r'^#define\s+' + name + r'.*', header, re.M).group())
        functions = '\n'.join(function(source, name) for name in [
            'mf1_capture_observation', 'nested_recover_core',
            'static_nested_recover_core', 'mf1_hardnested_nonces_acquire',
            'mf1_collect_observations',
        ])
        functions += '\n' + function((HF / 'nfca_commands.c').read_text(), 'mf1_collect_command')
        functions += '\n' + capture_wrapper()
        fixture = (Path(__file__).parent / 'tests/mifare_capture.c').read_text()
        fixture = fixture.replace('/* CAPTURE DECLARATIONS */', '\n'.join(declarations))
        fixture = fixture.replace('/* CAPTURE FUNCTIONS */', functions)
        run_fixture(fixture)

    def test_selection_frame_lengths_and_cascade_bounds(self):
        source = function((HF / 'rc522.c').read_text(), 'pcd_14a_reader_scan_once')
        fixture = (Path(__file__).parent / 'tests/mifare_selection.c').read_text()
        run_fixture(fixture.replace('/* SELECTION FUNCTION */', source))

    def test_irq_wait_uses_transaction_time_and_keeps_completed_replies(self):
        source = function((HF / 'rc522.c').read_text(), 'wait_for_irq')
        fixture = (Path(__file__).parent / 'tests/mifare_irq_wait.c').read_text()
        run_fixture(fixture.replace('/* WAIT FUNCTION */', source))

    def test_wakeup_deadlines_and_slow_fallback(self):
        source = (HF / 'rc522.c').read_text()
        functions = '\n'.join(function(source, name) for name in [
            'wait_for_irq', 'atqa_request',
            'pcd_14a_reader_scan_auto', 'pcd_14a_reader_scan_poll',
        ])
        fixture = (Path(__file__).parent / 'tests/mifare_wakeup.c').read_text()
        run_fixture(fixture.replace('/* CAPTURE FUNCTIONS */', functions))

    def test_session_lifecycle_and_command_cleanup(self):
        session = (HF / 'nfca_session.c').read_text()
        session = session[session.index('static bool active'):]
        source = (ROOT / 'firmware/application/src/app_cmd.c').read_text()
        functions = '\n'.join(function(source, name) for name in [
            'cmd_processor_nfca_session', 'before_hf_reader_run', 'after_hf_reader_run',
            'rf_only_command_allowed',
        ])
        shared = (HF / 'nfca_commands.c').read_text()
        functions = '\n'.join(function(shared, name) for name in [
            'nfca_prepare', 'nfca_finish', 'mf1_collect_command']) + '\n' + capture_wrapper() + '\n' + functions
        functions += '\n' + function((HF / 'hf_reader.c').read_text(), 'hf_reader_supports')
        fixture = (Path(__file__).parent / 'tests/mifare_session.c').read_text()
        fixture = fixture.replace('/* SESSION FUNCTIONS */', 'static bool ble_slave_rf_only(void) { return false; }\n' + session)
        fixture = fixture.replace('/* COMMAND FUNCTIONS */', functions)
        fixture = fixture.replace('/* DISPATCH FUNCTION */', function(source, 'on_data_frame_received'))
        run_fixture('#define PROJECT_CHAMELEON_ULTRA\n' + fixture)



if __name__ == '__main__':
    unittest.main()
