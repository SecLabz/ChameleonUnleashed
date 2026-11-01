"""Host regression checks for LF frame decoding using actual protocol sources."""
from pathlib import Path
import ctypes
import gzip
import json
import math
import random
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LF = ROOT / 'firmware/application/src/rfid/nfctag/lf'

def host_stubs(directory):
    (directory / 'nrf_pwm.h').write_text('''#pragma once
#include <stdint.h>
typedef struct { uint16_t channel_0,channel_1,channel_2,counter_top; } nrf_pwm_values_wave_form_t;
typedef struct {
    union { const nrf_pwm_values_wave_form_t *p_wave_form; } values;
    uint16_t length; uint32_t repeats,end_delay;
} nrf_pwm_sequence_t;
#define NRF_PWM_VALUES_LENGTH(a) (sizeof(a)/(sizeof(uint16_t)))
''')
    (directory / 'nordic_common.h').write_text('#define IS_SET(v,b) (((v) >> (b)) & 1u)\n#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\n#define SET_BIT(v,b) ((v) |= 1u << (b))\n')
    for name in ['nrf_log.h', 'nrf_log_ctrl.h', 'nrf_log_default_backends.h']:
        (directory / name).write_text('#define NRF_LOG_MODULE_REGISTER()\n')


class Demod(ctypes.Structure):
    _fields_ = [
        ('configuration', ctypes.c_uint32), ('clock', ctypes.c_uint16),
        ('offset', ctypes.c_uint16), ('modulation', ctypes.c_uint8),
        ('level_low', ctypes.c_uint16), ('level_high', ctypes.c_uint16),
        ('inverted', ctypes.c_bool),
    ]


class T55xxDecoder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        library = Path(cls.directory.name) / 'decoder.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-O2', '-shared', '-fPIC',
                        str(LF / 'protocols/t55xx_decode.c'), str(LF / 'utils/manchester.c'),
                        '-I', str(LF), '-o', str(library)], check=True)
        cls.decoder = ctypes.CDLL(str(library))
        cls.decoder.t55xx_detect_samples.argtypes = [ctypes.POINTER(ctypes.c_uint16), ctypes.c_size_t, ctypes.POINTER(Demod), ctypes.c_void_p]
        cls.decoder.t55xx_detect_samples.restype = ctypes.c_bool
        cls.decoder.t55xx_read_samples.argtypes = [ctypes.POINTER(ctypes.c_uint16), ctypes.c_size_t, ctypes.POINTER(Demod), ctypes.POINTER(ctypes.c_uint32)]
        cls.decoder.t55xx_read_samples.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    @staticmethod
    def waveform(word, clock, modulation, inverted=False):
        # Independent physical encoder: transitions/frequencies follow ATA5577C
        # table 5-6. Initial field transient is not part of the repeated word.
        values = [110] * 37
        phase = 0
        boundary = 0
        for index in range((12288 - 37) // clock):
            bit = (word >> (31 - index % 32)) & 1
            phase_changed = bit != phase
            if modulation == 1:
                phase = bit
            if modulation in (16, 24):
                boundary ^= 1
            for sample in range(clock):
                if modulation == 8:
                    level = bool(bit) ^ (sample >= clock // 2)
                elif modulation in (16, 24):
                    level = boundary ^ bool(bit and sample >= clock // 2)
                elif modulation in (4, 5, 6, 7):
                    periods = {4: (5, 8), 5: (10, 8), 6: (8, 5), 7: (8, 10)}[modulation]
                    level = math.sin(2 * math.pi * (index * clock + sample) / periods[bit]) >= 0
                elif modulation == 1:
                    # The reader's envelope filter removes the subcarrier.
                    # A phase transition produces a decaying signed excursion.
                    excursion = 0
                    if phase_changed:
                        excursion = (40 if bit else -40) * math.exp(-sample / 4)
                    value = round(110 + excursion)
                    values.append(220 - value if inverted else value)
                    continue
                else:
                    level = bit
                values.append(150 if bool(level) != inverted else 70)
            if modulation in (16, 24):
                boundary ^= bit
        return bytes(values)

    def detect(self, data):
        samples = (ctypes.c_uint16 * len(data))(*(value * 64 for value in data))
        demod = Demod()
        found = self.decoder.t55xx_detect_samples(samples, len(samples), ctypes.byref(demod), None)
        return found, demod

    def test_modulations_and_data_readback(self):
        for modulation in (8, 4, 5, 6, 7, 0, 16, 24, 1):
            for inverted in (False, True):
                with self.subTest(modulation=modulation, inverted=inverted):
                    config = 0x00140041 | (modulation << 12)
                    found, demod = self.detect(self.waveform(config, 64, modulation, inverted=inverted))
                    self.assertTrue(found)
                    self.assertEqual(demod.configuration, config)
                    data = self.waveform(0x1234ABCD, 64, modulation, inverted=inverted)
                    samples = (ctypes.c_uint16 * len(data))(*(value * 64 for value in data))
                    actual = ctypes.c_uint32()
                    self.assertTrue(self.decoder.t55xx_read_samples(samples, len(samples), ctypes.byref(demod), ctypes.byref(actual)))
                    self.assertEqual(actual.value, 0x1234ABCD)

    def test_clock_variants_and_psk_carriers(self):
        for index, clock in enumerate((8, 16, 32, 40, 50, 64, 100, 128)):
            config = (index << 18) | 0x8041
            found, demod = self.detect(self.waveform(config, clock, 8))
            self.assertTrue(found, clock)
            self.assertEqual(demod.configuration, config)
        for index, carrier in enumerate((2, 4, 8)):
            config = 0x00141041 | (index << 10)
            found, demod = self.detect(self.waveform(config, 64, 1))
            self.assertTrue(found, carrier)
            self.assertEqual(demod.configuration, config)

    def test_real_chameleon_captures_with_drifting_envelope(self):
        for filename, expected in [('t55xx_config_capture.bin.gz', 0x00148041),
                                   ('t55xx_viking_capture.bin.gz', 0x00088041)]:
            capture = gzip.decompress((ROOT / 'resource/tools/tests' / filename).read_bytes())
            found, demod = self.detect(capture)
            self.assertTrue(found)
            self.assertEqual(demod.configuration, expected)

    def test_nrz_long_ones_are_not_confused_with_transition_spikes(self):
        capture = gzip.decompress((ROOT / 'resource/tools/tests/t55xx_nrz_capture.bin.gz').read_bytes())
        found, demod = self.detect(capture[:12288])
        self.assertTrue(found)
        self.assertEqual(demod.configuration, 0x00080081)
        for index, expected in enumerate([0x00080081, 0xFFFF0102, 0x6D0721D8]):
            data = capture[index * 12288:(index + 1) * 12288]
            samples = (ctypes.c_uint16 * len(data))(*(value * 64 for value in data))
            actual = ctypes.c_uint32()
            self.assertTrue(self.decoder.t55xx_read_samples(samples, len(samples), ctypes.byref(demod), ctypes.byref(actual)))
            self.assertEqual(actual.value, expected)

    def test_real_psk1_phase_excursions_survive_the_filtered_adc(self):
        capture = gzip.decompress((ROOT / 'resource/tools/tests/t55xx_psk_capture.bin.gz').read_bytes())
        values = struct.unpack('<36864H', capture)
        samples = (ctypes.c_uint16 * 12288)(*values[:12288])
        demod = Demod()
        self.assertTrue(self.decoder.t55xx_detect_samples(samples, len(samples), ctypes.byref(demod), None))
        self.assertEqual(demod.configuration, 0x00081041)
        self.assertEqual(demod.modulation, 1)
        for block, expected in enumerate([0x00081041, 0x4944544B, 0x12345678]):
            samples = (ctypes.c_uint16 * 12288)(*values[block * 12288:(block + 1) * 12288])
            actual = ctypes.c_uint32()
            self.assertTrue(self.decoder.t55xx_read_samples(samples, len(samples), ctypes.byref(demod), ctypes.byref(actual)))
            self.assertEqual(actual.value, expected)
            # Excursion amplitude depends on the block's phase-change pattern
            # and coupling. Configuration thresholds must not fix its gain.
            scaled = [round(5500 + (value - 5500) / 2) for value in values[block * 12288:(block + 1) * 12288]]
            samples = (ctypes.c_uint16 * 12288)(*scaled)
            self.assertTrue(self.decoder.t55xx_read_samples(samples, len(samples), ctypes.byref(demod), ctypes.byref(actual)))
            self.assertEqual(actual.value, expected)
        long_run = gzip.decompress((ROOT / 'resource/tools/tests/t55xx_psk_long_capture.bin.gz').read_bytes())
        samples = (ctypes.c_uint16 * 12288)(*struct.unpack('<12288H', long_run))
        self.assertTrue(self.decoder.t55xx_read_samples(samples, len(samples), ctypes.byref(demod), ctypes.byref(actual)))
        self.assertEqual(actual.value, 0xFF800000)

    def test_constant_nrz_and_psk_words_remain_unknown(self):
        for modulation in [0, 1]:
            found, demod = self.detect(self.waveform(0x00080081 | modulation << 12, 32, modulation))
            self.assertTrue(found)
            for word in [0, 0xFFFFFFFF]:
                data = self.waveform(word, 32, modulation)
                samples = (ctypes.c_uint16 * len(data))(*(value * 64 for value in data))
                actual = ctypes.c_uint32()
                self.assertFalse(self.decoder.t55xx_read_samples(samples, len(samples), ctypes.byref(demod), ctypes.byref(actual)))

    def test_silence_noise_and_ordinary_broadcast_are_inconclusive(self):
        randomizer = random.Random(37)
        for data in [bytes(12288), bytes([128]) * 12288,
                     bytes(randomizer.randrange(256) for _ in range(12288)),
                     self.waveform(0xFF800000, 64, 8)]:
            self.assertFalse(self.detect(data)[0])


class LfProtocols(unittest.TestCase):
    def test_web_ioprox_fields_match_the_firmware_codec(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            host_stubs(directory)
            library = directory / 'ioprox.so'
            subprocess.run(['cc', '-std=c11', '-O2', '-shared', '-fPIC',
                            '-I', str(directory), '-I', str(LF), '-I', str(LF / 'utils'),
                            '-I', str(LF.parent), '-I', str(LF.parent.parent),
                            str(LF / 'protocols/ioprox.c'), str(LF / 'utils/fskdemod.c'),
                            '-lm', '-o', str(library)], check=True)
            codec = ctypes.CDLL(str(library))
            codec.ioprox_encode_params_to_data.argtypes = [ctypes.c_uint8, ctypes.c_uint8,
                ctypes.c_uint16, ctypes.POINTER(ctypes.c_uint8)]
            codec.ioprox_encode_params_to_data.restype = ctypes.c_bool
            cases = [(0, 0, 0), (255, 255, 65535), (1, 42, 12345)]
            script = '''import { encodeLfFields } from './.cache/web/assets/modules/documents/lf.mjs';
import { readFileSync } from 'node:fs';
const cases = JSON.parse(readFileSync(0, 'utf8'));
process.stdout.write(JSON.stringify(cases.map(([version, facility, number]) =>
    encodeLfFields(201, {version: String(version), facility: String(facility), number: String(number)}))));'''
            actual = json.loads(subprocess.check_output(['node', '--input-type=module', '-e', script],
                cwd=ROOT, input=json.dumps(cases).encode()))
            for parameters, encoded in zip(cases, actual, strict=True):
                expected = (ctypes.c_uint8 * 16)()
                self.assertTrue(codec.ioprox_encode_params_to_data(*parameters, expected))
                self.assertEqual(encoded, list(expected), parameters)

    def test_electra_suffix_wins_over_its_em410x_prefix(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            host_stubs(directory)
            (directory / 'bsp_time.h').write_text('''#pragma once
#include <stdint.h>
typedef struct { volatile uint32_t time; } autotimer;
autotimer *bsp_obtain_timer(uint32_t initial);
void bsp_return_timer(autotimer *timer);
#define NO_TIMEOUT_1MS(timer, limit) ((timer)->time <= (limit))
''')
            (directory / 'ble_main.h').write_text('#include <stddef.h>\n#include <stdint.h>\ntypedef void (*lf_adc_callback_t)(int16_t *, size_t);\n')
            (directory / 'nrfx_pwm.h').write_text('typedef struct {} nrfx_pwm_t;\n')
            reader = ROOT / 'firmware/application/src/rfid/reader/lf'
            binary = directory / 'electra'
            sources = [ROOT / 'resource/tools/tests/lf_em410x_reader.c', reader / 'lf_em410x_data.c',
                       LF / 'protocols/em410x.c', LF / 'utils/manchester.c',
                       LF.parent.parent / 'parity.c']
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(directory), '-I', str(LF),
                            '-I', str(LF / 'utils'), '-I', str(LF.parent),
                            '-I', str(LF.parent.parent), '-I', str(reader),
                            *map(str, sources), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_t55xx_programming_failure_boundaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            host_stubs(directory)
            reader = ROOT / 'firmware/application/src/rfid/reader/lf'
            binary = directory / 't55xx'
            sources = [ROOT / 'resource/tools/tests/t55xx_program.c', reader / 'lf_t55xx.c',
                       LF.parent.parent / 'hex_utils.c', LF.parent.parent / 'parity.c']
            sources += [LF / ('protocols/' + name + '.c') for name in
                        ['em410x', 'hidprox', 'wiegand', 'ioprox', 'pac', 'viking', 'jablotron', 'idteck']]
            sources += list((LF / 'utils').glob('*.c'))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-Wno-unused-function',
                            '-fsanitize=address,undefined', '-I', str(directory), '-I', str(LF),
                            '-I', str(LF / 'protocols'), '-I', str(LF / 'utils'),
                            '-I', str(LF.parent), '-I', str(LF.parent.parent),
                            '-I', str(reader), '-I', str(ROOT / 'firmware/application/src'),
                            *map(str, sources), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_viking_handles_polarity_and_rejects_corrupt_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            host_stubs(directory)
            (directory / 'bsp_time.h').write_text('''#pragma once
#include <stdint.h>
typedef struct { volatile uint32_t time; } autotimer;
autotimer *bsp_obtain_timer(uint32_t initial);
void bsp_return_timer(autotimer *timer);
#define NO_TIMEOUT_1MS(timer, limit) ((timer)->time <= (limit))
''')
            (directory / 'bsp_delay.h').write_text('#include <stdint.h>\nvoid bsp_delay_ms(uint32_t);\n')
            (directory / 'bsp_wdt.h').write_text('void bsp_wdt_feed(void);\n')
            (directory / 'ble_main.h').write_text('#include <stddef.h>\n#include <stdint.h>\ntypedef void (*lf_adc_callback_t)(int16_t *, size_t);\n')
            (directory / 'nrfx_pwm.h').write_text('typedef struct {} nrfx_pwm_t;\n')
            (directory / 'nrfx_saadc.h').write_text('#include <stdint.h>\ntypedef int16_t nrf_saadc_value_t;\n')
            reader = ROOT / 'firmware/application/src/rfid/reader/lf'
            binary = directory / 'viking'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Werror', '-fsanitize=address,undefined',
                '-I', str(directory), '-I', str(LF), '-I', str(LF / 'protocols'),
                '-I', str(LF.parent), '-I', str(LF.parent.parent),
                '-I', str(reader), '-I', str(reader.parent),
                str(ROOT / 'resource/tools/tests/lf_viking.c'),
                str(reader / 'lf_viking_data.c'), str(LF / 'protocols/viking.c'),
                str(LF / 'utils/manchester.c'),
                '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
