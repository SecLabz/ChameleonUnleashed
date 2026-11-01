"""Exercise production maintenance with RF stubs and the existing write peer."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from native_test import function

ROOT = Path(__file__).resolve().parents[2]
HF = ROOT / 'firmware/application/src/rfid/reader/hf'


class MaintenanceTest(unittest.TestCase):
    def test_selection_cleanup_guards_and_backdoor_trailers(self):
        fixture = (ROOT / 'resource/tools/tests/mifare_write.c').read_text().split('/* WRITE FUNCTIONS */')[0]
        fixture = fixture.replace('static uint16_t pcd_14a_reader_scan_auto', '''typedef struct {
    int8_t forcebcc, forcecl2, forcecl3, forcerats;
} hf14a_config_t;
static hf14a_config_t config = {0, 0, 0, 1};
static hf14a_config_t *get_hf14a_config(void) { return &config; }
static void set_hf14a_config(const hf14a_config_t *value) { config = *value; }
static void pcd_14a_reader_antenna_off(void) { unauthenticated = true; }
static void pcd_14a_reader_mf1_unauth(void) { unauthenticated = true; }
static uint16_t pcd_14a_reader_scan_auto''', 1)
        fixture = fixture.replace('    if (lost_activation && writes) {', '''    if (!config.forcebcc && (memory[0][0] ^ memory[0][1] ^ memory[0][2] ^ memory[0][3]) != memory[0][4]) {
        return STATUS_HF_ERR_BCC;
    }
    if (lost_activation && writes) {''', 1)
        fixture = fixture.replace('static uint16_t pcd_14a_reader_mf1_read', '''static uint16_t pcd_14a_reader_magic_unlock(bool second) {
    (void)second;
    return pcd_14a_reader_gen1a_unlock();
}
static uint16_t pcd_14a_reader_mf1_read''', 1)
        fixture = fixture.replace('assert(milliseconds == 8);', 'assert(milliseconds == 5 || milliseconds == 8);')
        source = (HF / 'nfca_commands.c').read_text()
        functions = '\n'.join(function(source, name) for name in (
            'classic_access_valid', 'trailer_set', 'maintenance_open', 'maintenance_reply',
            'maintenance_run', 'maintenance'))
        body = (ROOT / 'resource/tools/tests/classic_maintenance.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'maintenance.c'
            binary = Path(directory) / 'maintenance'
            path.write_text(fixture + functions + body)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(HF),
                '-I', str(ROOT / 'firmware/application/src'), str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
