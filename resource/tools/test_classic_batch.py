"""Exercise actual batched reader dispatch with sanitizer-checked RF mocks."""
from copy import deepcopy
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from native_test import function

ROOT = Path(__file__).resolve().parents[2]

class ClassicBatch(unittest.TestCase):
    def test_serial_dispatch_uses_the_same_batch_protocol_and_requires_reader_mode(self):
        source = (ROOT / 'firmware/application/src/app_cmd.c').read_text()
        processor = function(source, 'cmd_processor_classic_batch')
        self.assertRegex(source, r'\{\s*DATA_CMD_MF1_BATCH,\s*NULL,\s*cmd_processor_classic_batch,\s*NULL\s*\}')
        fixture = '''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "app_status.h"
#define DEVICE_MODE_READER 1
typedef struct { uint16_t cmd, status, size; uint8_t bytes[512]; } data_frame_tx_t;
static int mode, calls;
static data_frame_tx_t frame;
static int get_device_mode(void) { return mode; }
static data_frame_tx_t *data_frame_make(uint16_t cmd, uint16_t status, uint16_t size, const uint8_t *data) {
    frame.cmd=cmd; frame.status=status; frame.size=size;
    if(size) memcpy(frame.bytes,data,size);
    return &frame;
}
static uint16_t classic_batch_request(const uint8_t *body, size_t size, uint8_t *output, size_t capacity, uint16_t *length) {
    assert(capacity==512 && size==3 && body[0]==1);
    calls++; memcpy(output,body,size); *length=size; return 0;
}
/* PROCESSOR */
int main(void) {
    uint8_t request[]={1,0,1};
    cmd_processor_classic_batch(2042,0,3,request);
    assert(frame.status==STATUS_DEVICE_MODE_ERROR && !calls && !frame.size);
    mode=1;
    cmd_processor_classic_batch(2042,0,3,request);
    assert(frame.cmd==2042 && !frame.status && frame.size==3 && calls==1);
    assert(!memcmp(frame.bytes,request,3));
}
'''.replace('/* PROCESSOR */', processor)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'serial.c').write_text(fixture)
            binary = directory / 'serial'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(ROOT / 'firmware/application/src'),
                str(directory / 'serial.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_cooperative_jobs_backpressure_cancel_and_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            binary = directory / 'job'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                '-I', str(directory), '-I', str(ROOT / 'firmware/application/src'),
                '-I', str(ROOT / 'firmware/application/src/rfid/reader/hf'),
                str(ROOT / 'resource/tools/tests/classic_job.c'),
                str(ROOT / 'firmware/application/src/rfid/reader/hf/classic_job.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_bounds_identity_partial_results_and_fresh_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'batch'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                '-I', str(ROOT / 'firmware/application/src'),
                '-I', str(ROOT / 'firmware/application/src/rfid/reader/hf'),
                str(ROOT / 'resource/tools/tests/classic_batch.c'),
                str(ROOT / 'firmware/application/src/rfid/reader/hf/classic_batch.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

class SimulatedClassicBatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('web_simulator', ROOT / 'software/web/tests/serve.py')
        module = importlib.util.module_from_spec(spec)
        # Protocol tests do not need generated web assets or an HTTP listener.
        with patch.object(Path, 'read_text', return_value='{"baseUrl":"/","projectVersion":"test"}'):
            spec.loader.exec_module(module)
        cls.handler = module.SimulatedCard

    def setUp(self):
        class Card(self.handler):
            classic_memory = deepcopy(self.handler.classic_memory)
            tag_kind = 'classic'

        self.card = Card.__new__(Card)

    def batch(self, kind, entries, uid=b'\x01\x02\x03\x04'):
        body = bytes([1, kind, len(uid)]) + uid + bytes([len(entries)]) + b''.join(entries)
        return self.card.classic_batch(body)

    def test_readable_key_b_cannot_authenticate_but_a_protected_key_b_can(self):
        entry = bytes([0, 0x61]) + b'\xff' * 6
        self.assertEqual(self.batch(0, [entry]), (0, bytes([1, 0, 1, 0, 6, 0, 7]) + bytes(7)))
        self.assertEqual(self.batch(1, [entry]), (0, bytes([1, 1, 1, 0, 6, 0, 0])))
        memory = type(self.card).classic_memory
        memory[3] = memory[3][:6] + bytes.fromhex('7f078869') + memory[3][10:]
        self.assertEqual(self.batch(0, [entry]), (0, bytes([1, 0, 1, 0, 0, 0, 7, 1]) + b'\xff' * 6))

    def test_large_sector_geometry_and_reply_capacity(self):
        key = bytes([0x60]) + b'\xff' * 6
        type(self.card).classic_memory[128] = bytes(range(16))
        status, response = self.batch(1, [bytes([32]) + key])
        self.assertEqual(status, 0)
        self.assertEqual(len(response), 265)
        self.assertEqual(response[:9], bytes([1, 1, 1, 0, 0, 1, 2, 255, 255]))
        self.assertEqual(response[9:25], bytes(range(16)))
        self.assertEqual(response[-16:-10], bytes(6))
        for sectors in ([32, 33], list(range(8))):
            with self.subTest(sectors=sectors):
                self.assertEqual(self.batch(1, [bytes([n]) + key for n in sectors]), (0x60, b''))

    def test_changed_uid_rejects_checks_and_stops_sector_reads(self):
        entries = [bytes([n, 0x60]) + b'\xff' * 6 for n in (0, 4)]
        self.assertEqual(self.batch(0, entries, uid=b'\x04\x03\x02\x01'), (1, b''))
        self.assertEqual(self.batch(0, entries, uid=type(self.card).classic_memory[0][:7]), (1, b''))
        self.assertEqual(self.batch(1, entries, uid=b'\x04\x03\x02\x01'),
                         (0, bytes([1, 1, 1, 0, 1, 0, 0])))


if __name__ == '__main__':
    unittest.main()
