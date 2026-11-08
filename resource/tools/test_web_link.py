"""Exercise the temporary NDEF tag with real image layouts and slot routing."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from native_test import function

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'firmware/application/src'
TAGS = SRC / 'rfid/nfctag'


class WebLinkTests(unittest.TestCase):
    def test_ndef_slot_restoration_leds_and_exit_conditions(self):
        source = (TAGS / 'tag_emulation.c').read_text()
        ntag = (TAGS / 'hf/nfc_mf0_ntag.c').read_text()
        fixture = (ROOT / 'resource/tools/tests/web_link.c').read_text()
        state = source[source.index('APP_TIMER_DEF(m_web_link_timer)'):
                       source.index('static tag_specific_type_t tag_specific_type_old2new_lf_values')]
        callbacks = '\n'.join(function(source, name) for name in (
            'tag_emulation_web_link_active', 'web_link_timeout', 'web_link_end',
            'tag_emulation_web_link_stop', 'tag_emulation_web_link_start',
            'tag_emulation_web_link_poll', 'tag_emulation_sense_switch',
            'tag_emulation_sense_switch_all', 'tag_emulation_sense_run',
            'tag_emulation_sense_end', 'sync_snapshot_buffer'))
        geometry = '\n'.join(function(ntag, name) for name in (
            'nfc_tag_mf0_ntag_get_nr_pages_by_tag_type', 'get_total_pages_by_tag_type',
            'get_information_size_by_tag_type', 'nfc_tag_mf0_ntag_data_loadcb',
            'nfc_tag_mf0_ntag_data_savecb'))
        sleep = function((SRC / 'utils/syssleep.c').read_text(), 'sleep_timer_start')
        fixture = fixture.replace('/* LINK_STATE */', state)
        fixture = fixture.replace('/* NTAG_CALLBACKS */', geometry)
        fixture = fixture.replace('/* LINK_CALLBACKS */', callbacks)
        fixture = fixture.replace('/* SLEEP_CALLBACK */', sleep)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / 'app_util.h').write_text('#define STATIC_ASSERT(c) _Static_assert(c,"layout")\n')
            (path / 'utils.h').write_text('#define PACKED __attribute__((packed))\n')
            (path / 'link.c').write_text(fixture)
            binary = path / 'link'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fshort-enums',
                '-DPROJECT_CHAMELEON_ULTRA', '-DNFC_MF1_FAST_SIM',
                '-fsanitize=address,undefined', '-I', str(path), '-I', str(TAGS),
                '-I', str(TAGS / 'hf'), '-I', str(SRC / 'rfid/reader'),
                str(path / 'link.c'), str(SRC / 'rfid/reader/rf_workspace.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
