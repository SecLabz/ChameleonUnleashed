"""Verify ST25TB decoding with synthetic frames and the lossless captures.zip corpus.

Archive members are original 76-byte ADC bank records, also usable by st25tb/replay.sh
after extraction with `python3 -m zipfile -e captures.zip DIRECTORY`.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from zipfile import ZipFile

from native_test import function

ROOT = Path(__file__).resolve().parents[2]
RFID = ROOT / 'firmware/application/src/rfid'
CAPTURES = ROOT / 'resource/tools/tests/st25tb/captures.zip'


def swap_axes(data):
    data = bytearray(data)
    swap = bytes(((value & 15) << 4) | (value >> 4) for value in range(256))
    for offset in range(0, len(data), 76):
        data[offset:offset + 65] = data[offset:offset + 65].translate(swap)
        data[offset + 67] = swap[data[offset + 67]]
    return data


class ReceiverTests(unittest.TestCase):
    def test_repair_class_generated(self):
        subprocess.run(['python3', str(ROOT / 'resource/tools/generate_st25tb_repair.py'),
                        '--check'], check=True)

    def test_repair_choices_and_budget(self):
        self.run_fixture('st25tb_repair', None)

    def test_model_reselection(self):
        self.run_fixture("st25tb_model", "model")

    def test_captured_frames_decode(self):
        self.run_fixture("st25tb_receiver", "receiver", count_crc=True)

    def test_capture_corpus(self):
        expected = {
            'selection/caps/crc1.bin': '[08 02 95 e2]',
            'selection/caps/crc10.bin': '[06 00 97 5b]',
            'selection/caps/crc11.bin': '[0b ab 4e]',
            'selection/caps/crc12.bin': '[08 00 87 c1]',
            'selection/caps/crc13.bin': '[0e a1 d4 21]',
            'selection/caps/crc14.bin': '[0e a1 d4 21]',
            'selection/caps/crc15.bin': '[0b ab 4e]',
            'selection/caps/crc16.bin': '[0b ab 4e]',
            'selection/caps/crc17.bin': '[0e a1 d4 21]',
            'selection/caps/crc18.bin': '[08 02 95 e2]',
            'selection/caps/crc19.bin': '[08 04 a3 87]',
            'selection/caps/crc2.bin': '[06 00 97 5b]',
            'selection/caps/crc20.bin': '[08 06 b1 a4]',
            'selection/caps/crc21.bin': '[08 00 87 c1]',
            'selection/caps/crc22.bin': '[0e a1 d4 21]',
            'selection/caps/crc23.bin': '[08 06 b1 a4]',
            'selection/caps/crc24.bin': '[06 00 97 5b]',
            'selection/caps/crc25.bin': '[06 00 97 5b]',
            'selection/caps/crc26.bin': '[08 04 a3 87]',
            'selection/caps/crc3.bin': '[0e a1 d4 21]',
            'selection/caps/crc4.bin': '[06 00 97 5b]',
            'selection/caps/crc5.bin': '[08 0e f9 28]',
            'selection/caps/crc6.bin': '[08 04 a3 87]',
            'selection/caps/crc7.bin': '[06 00 97 5b]',
            'selection/caps/crc8.bin': '[0e a1 d4 21]',
            'selection/caps/crc9.bin': '[08 03 1c f3]',
            'selection/caps/rx1.bin': '[06 00 97 5b]',
            'selection/caps/rx2.bin': '[05 00 00 71 ff]',
            'selection/caps/rx3.bin': '[05 00 00 71 ff]',
            'selection/caps/rx4.bin': '[06 00 97 5b]',
            'selection/caps/rx5.bin': '[06 00 97 5b]',
            'selection/caps/rx6.bin': '[06 00 97 5b]',
            'selection/caps/rx7.bin': '[06 00 97 5b]',
            'selection/caps/rx8.bin': '[06 00 97 5b]',
            'selection/post/miss1.bin': '[0b ab 4e]',
            'selection/post/miss2.bin': '[0e fc b4 a8]',
            'selection/post/miss3.bin': '[0b ab 4e]',
            'selection/post/ok1.bin': '[0e a1 d4 21]',
            'selection/post/ok2.bin': '[0e a1 d4 21]',
            'selection/post/ok3.bin': '[0e a1 d4 21]',
            'edges/get-uid-start-gap.bin': '[0b ab 4e]',
            'edges/get-uid-peer-sof.bin': '[0b ab 4e]',
            'edges/read-ff-ringing.bin': '[08 ff ff ce]',
            'edges/read-ff-peer-progress.bin': '[08 ff ff ce]',
            'edges/initiate-peer-order.bin': '[06 00 97 5b]',
            'eof/read255-peer-eof.bin': '[08 ff ff ce]',
            'crc/crc1.bin': 'LOST',
            'crc/crc2.bin': 'LOST',
            'crc/crc3.bin': 'LOST',
            'lost/m1.bin': '[0e a1 d4 21]',
            'lost/m16.bin': '[08 ff ff ce]',
            'lost/m6.bin': '[08 0e f9 28]',
            'lost/m7.bin': '[08 05 2a 96]',
            'lost/m9.bin': '[08 ff ff ce]',
        }
        with ZipFile(CAPTURES) as archive:
            self.assertEqual(set(archive.namelist()), set(expected))
            captures = {name: archive.read(name) for name in sorted(expected)}
        source = ROOT / 'resource/tools/st25tb/replay.c'
        with tempfile.TemporaryDirectory() as directory:
            cases = []
            for name, data in captures.items():
                path = Path(directory) / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                cases.append((path, expected[name]))
            for index, (name, data) in enumerate(captures.items()):
                # Exchange I/Q in ADC bytes and carry; preserve all bank metadata.
                swapped = Path(directory) / f'swapped-{index}.bin'
                swapped.write_bytes(swap_axes(data))
                cases.append((swapped, expected[name]))
            output = self.run_fixture('st25tb_captures', 'receiver', source=source,
                                      arguments=[path for path, _ in cases])
            lines = output.splitlines()
            self.assertEqual(len(lines), len(cases) + 1)
            for (path, wanted), line in zip(cases, lines):
                with self.subTest(capture=path):
                    self.assertEqual(line[len(str(path)):].strip(), wanted)

        # These old post-reply recordings predate the distinct restart marker.
        # Apply the current reset + settle sequence without changing any ADC samples.
        with tempfile.TemporaryDirectory() as directory:
            settled = []
            for name in ('miss1', 'miss3'):
                data = bytearray(captures[f'selection/post/{name}.bin'])
                self.assertEqual(data[68], 2)
                data[68] = 3
                path = Path(directory) / f'{name}.bin'
                path.write_bytes(data)
                settled.append(path)
            output = self.run_fixture('st25tb_settled', 'receiver', source=source, arguments=settled)
            lines = output.splitlines()
            self.assertEqual(len(lines), 3)
            for path, line in zip(settled, lines):
                self.assertEqual(line[len(str(path)):].strip(), '[0b ab 4e]')

    def test_peer_eof_requires_measured_edges(self):
        with ZipFile(CAPTURES) as archive:
            original = archive.read('eof/read255-peer-eof.bin')
        positions = {}
        samples = {}
        for offset in range(0, len(original), 76):
            if original[offset + 68] != 1:
                continue
            tick = int.from_bytes(original[offset + 72:offset + 76], 'little')
            for index in range(original[offset + 65], 64, 2):
                positions[tick] = offset + index
                samples[tick] = original[offset + index:offset + index + 2]
                tick += 1

        cases = [('original', original, '[08 ff ff ce]')]
        for control in ('remove-peer-stop', 'remove-eof', 'short-eof', 'long-eof'):
            data = bytearray(original)
            for tick, position in positions.items():
                first, second = samples[tick]
                if control == 'remove-peer-stop' and 15279 <= tick <= 15290:
                    first = (first & 0xf0) | 15
                    second = (second & 0xf0) | 15
                if control == 'remove-eof' and 15326 <= tick <= 15342:
                    first = 255
                    second = 255
                if control in ('short-eof', 'long-eof'):
                    shift = -5 if control == 'short-eof' else 5
                    if 15321 <= tick <= 15347:
                        first = 255
                        second = 255
                    if 15326 <= tick - shift <= 15342:
                        first, second = samples[tick - shift]
                data[position:position + 2] = bytes((first, second))
            cases.append((control, data, 'LOST'))

        # Keep every ADC value; place the measured stop edge just before wrap.
        wrapped = bytearray(original)
        shift = 0xfffffff0 - 15282
        for offset in range(0, len(wrapped), 76):
            if wrapped[offset + 68] == 1:
                tick = int.from_bytes(wrapped[offset + 72:offset + 76], 'little')
                wrapped[offset + 72:offset + 76] = ((tick + shift) & 0xffffffff).to_bytes(4, 'little')
        cases.append(('wrapped', wrapped, '[08 ff ff ce]'))

        with tempfile.TemporaryDirectory() as directory:
            files = []
            for name, data, expected in cases:
                for swapped in (False, True):
                    path = Path(directory) / f'{name}-{swapped}.bin'
                    path.write_bytes(swap_axes(data) if swapped else data)
                    files.append((path, expected))
            output = self.run_fixture('peer_eof_controls', 'receiver', source=ROOT / 'resource/tools/st25tb/replay.c',
                                      arguments=[path for path, _ in files])
            lines = output.splitlines()
            self.assertEqual(len(lines), len(files) + 1)
            for (path, expected), line in zip(files, lines):
                with self.subTest(capture=path):
                    self.assertEqual(line[len(str(path)):].strip(), expected)

    def test_slot_image_contract(self):
        source = function((ROOT / 'firmware/application/src/app_cmd.c').read_text(),
                          'cmd_processor_st25tb_get_image')
        fixture = (ROOT / 'resource/tools/tests/st25tb_image.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            test = path / 'image.c'
            test.write_text(fixture.replace('/* IMAGE COMMAND */', source))
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                '-I', str(RFID), str(test), '-o', str(path / 'image'),
            ], check=True)
            subprocess.run([str(path / 'image')], check=True, timeout=10)

    def test_image_write_preserves_unsaved_lf(self):
        st25tb = (RFID / 'nfctag/hf/nfc_st25tb.c').read_text()
        storage = (RFID / 'nfctag/tag_emulation.c').read_text()
        fixture = (ROOT / 'resource/tools/tests/st25tb_image_write.c').read_text()
        callbacks = '\n'.join(function(st25tb, name) for name in (
            'nfc_st25tb_data_loadcb', 'nfc_st25tb_data_savecb'))
        functions = '\n'.join(function(storage, name) for name in (
            'tag_emulation_load_by_buffer', 'load_data_by_tag_type',
            'save_data_by_tag_type', 'tag_emulation_load_data'))
        fixture = fixture.replace('/* ST25TB CALLBACKS */', callbacks)
        fixture = fixture.replace('/* SLOT STORAGE */', functions)
        fixture = fixture.replace('/* ST25TB SETTER */', function(st25tb, 'nfc_st25tb_set_image'))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'image_write.c'
            source.write_text(fixture)
            self.run_fixture('st25tb_image_write', 'model', source=source)

    def run_fixture(self, fixture, implementation, *, source=None, arguments=(), count_crc=False):
        if source is None:
            source = ROOT / f"resource/tools/tests/{fixture}.c"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / fixture
            compiler = [
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                '-fsanitize=address,undefined', '-I', str(RFID),
            ]
            implementation_sources = []
            if implementation is not None:
                implementation_sources.append(RFID / f"st25tb/{implementation}.c")
            if count_crc:
                counted = Path(directory) / 'receiver.o'
                subprocess.run([
                    *compiler, '-Diso15693_crc16=st25tb_test_crc16', '-c',
                    str(implementation_sources[0]), '-o', str(counted),
                ], check=True)
                implementation_sources = [counted]
            subprocess.run([
                *compiler,
                str(source),
                *(str(path) for path in implementation_sources), str(RFID / 'iso15693/frame.c'),
                '-lm', '-o', str(binary),
            ], check=True)
            result = subprocess.run([str(binary), *(str(path) for path in arguments)],
                                    check=True, timeout=10, capture_output=bool(arguments), text=True)
            return result.stdout


if __name__ == '__main__':
    unittest.main()
