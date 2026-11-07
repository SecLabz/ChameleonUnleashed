"""Exercise actual batched reader dispatch with sanitizer-checked RF mocks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class ClassicBatch(unittest.TestCase):
    def test_cooperative_jobs_streaming_backpressure_cancel_and_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'ble_main.h').write_text('#include <stdbool.h>\n#include <stdint.h>\nbool ble_stream_response(uint8_t *,uint16_t);\n')
            binary = directory / 'job'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                '-I', str(directory), '-I', str(ROOT / 'firmware/application/src'),
                '-I', str(ROOT / 'firmware/application/src/net'),
                '-I', str(ROOT / 'firmware/application/src/rfid/reader/hf'),
                str(ROOT / 'resource/tools/tests/classic_job.c'),
                str(ROOT / 'firmware/application/src/net/classic_job.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_bounds_identity_partial_results_and_fresh_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'batch'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                '-I', str(ROOT / 'firmware/application/src'),
                '-I', str(ROOT / 'firmware/application/src/net'),
                '-I', str(ROOT / 'firmware/application/src/rfid/reader/hf'),
                str(ROOT / 'resource/tools/tests/classic_batch.c'),
                str(ROOT / 'firmware/application/src/net/classic_batch.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
