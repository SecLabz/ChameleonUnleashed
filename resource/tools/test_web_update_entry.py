"""Exercise application-to-recovery entry with simulated RF interrupts."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from native_test import function

ROOT = Path(__file__).resolve().parents[2]


class WebUpdateEntryTests(unittest.TestCase):
    def test_status_and_entry_leave_active_rf_undisturbed(self):
        net = ROOT / 'firmware/application/src/net'
        source = (net / 'update_api.c').read_text()
        production = '\n'.join(function(source, name) for name in (
            'web_update_entering', 'bootloader_present', 'rf_busy', 'pause_emulation',
            'web_update_info', 'web_update_application_request', 'web_update_poll'))
        fixture = (ROOT / 'resource/tools/tests/web_update_entry.c').read_text()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / 'entry.c'
            binary = directory / 'entry'
            path.write_text(fixture.replace('/* PRODUCTION_ENTRY */', production))
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                '-fsanitize=address,undefined', '-I', str(net),
                '-I', str(ROOT / 'firmware/application/src'), '-I', str(ROOT / 'firmware/common'), str(path), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
