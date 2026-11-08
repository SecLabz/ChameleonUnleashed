"""Guard mutation dispatch separately from the real RATS peer in magic tests."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from native_test import function

ROOT = Path(__file__).resolve().parents[2]
HF = ROOT / 'firmware/application/src/rfid/reader/hf'


class CapacityTest(unittest.TestCase):
    def test_two_k_data_block0_and_trailer_require_capacity_evidence(self):
        fixture = (ROOT / 'resource/tools/tests/mifare_write.c').read_text()
        source = (HF / 'nfca_commands.c').read_text()
        functions = '\n'.join(function(source, name) for name in (
            'classic_model_check', 'write_user_unit', 'block0_set',
            'classic_access_valid', 'trailer_set'))
        stubs = '''
static uint16_t type2_model_check(uint8_t model, const uint8_t *uid) {
    (void)model; (void)uid; assert(!"Classic must not use Type 2"); return STATUS_PAR_ERR;
}
static uint16_t type2_password_auth(const uint8_t *key) {
    (void)key; assert(!"Classic must not use Type 2"); return STATUS_PAR_ERR;
}
static uint16_t pcd_14a_reader_mf0_ulc_auth(uint8_t *key) {
    (void)key; assert(!"Classic must not use Type 2"); return STATUS_PAR_ERR;
}
static uint16_t pcd_14a_reader_mf0_ult_write_page(uint8_t page, uint8_t *bytes) {
    (void)page; (void)bytes; assert(!"Classic must not use Type 2"); return STATUS_PAR_ERR;
}
'''
        body = (ROOT / 'resource/tools/tests/mifare_capacity.c').read_text()
        program = fixture.split('/* WRITE FUNCTIONS */')[0] + stubs + functions + function(fixture, 'reset') + body
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capacity.c'
            binary = Path(directory) / 'capacity'
            path.write_text(program)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(HF),
                '-I', str(ROOT / 'firmware/application/src'), str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
