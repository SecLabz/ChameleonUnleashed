#!/usr/bin/env python3
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from crypto1 import Crypto1  # noqa: E402

# Synthetic fixed vectors from random.Random(0xC1F17E57), calculated with
# software/src/crypto1.c (independent of the Python implementation under test).
class TestCrypto1(unittest.TestCase):

    def test_reader_three_pass_auth(self):
        uid, nt, nr, atEnc = 0x3935CC65, 0x7BF56B50, 0x75CA21CC, 0xD9A573DD
        reader = Crypto1()
        reader.key = '676AA760A91F'
        ks0 = reader.lfsr48_u32(uid ^ nt, False)
        self.assertEqual(ks0, 0x50431C98, 'ks0 assert failed')
        ks1 = reader.lfsr48_u32(nr, False)
        self.assertEqual(ks1, 0xC26CB20A, 'ks1 assert failed')
        nrEnc = nr ^ ks1
        self.assertEqual(nrEnc, 0xB7A693C6, 'nrEnc assert failed')
        ar = Crypto1.prng_next(nt, 64)
        self.assertEqual(ar, 0xC6018169, 'ar assert failed')
        ks2 = reader.lfsr48_u32(0, False)
        self.assertEqual(ks2, 0xDE41E75C, 'ks2 assert failed')
        arEnc = ar ^ ks2
        self.assertEqual(arEnc, 0x18406635, 'arEnc assert failed')
        ks3 = reader.lfsr48_u32(0, False)
        self.assertEqual(ks3, 0x44D09670, 'ks3 assert failed')
        at = atEnc ^ ks3
        nt96 = Crypto1.prng_next(nt, 96)
        self.assertEqual(at, nt96, 'at assert failed')

    def test_tag_three_pass_auth(self):
        uid, nt, nrEnc, arEnc = 0x3935CC65, 0x7BF56B50, 0xB7A693C6, 0x18406635
        tag = Crypto1()
        tag.key = '676AA760A91F'
        ks0 = tag.lfsr48_u32(uid ^ nt, False)
        self.assertEqual(ks0, 0x50431C98, 'ks0 assert failed')
        ks1 = tag.lfsr48_u32(nrEnc, True)
        self.assertEqual(ks1, 0xC26CB20A, 'ks1 assert failed')
        nr = ks1 ^ nrEnc
        self.assertEqual(nr, 0x75CA21CC, 'nr assert failed')
        ks2 = tag.lfsr48_u32(0, False)
        self.assertEqual(ks2, 0xDE41E75C, 'ks2 assert failed')
        ar = ks2 ^ arEnc
        self.assertEqual(ar, 0xC6018169, 'ar assert failed')
        at = Crypto1.prng_next(nt, 96)
        self.assertEqual(at, 0x9D75E5AD, 'at assert failed')
        ks3 = tag.lfsr48_u32(0, False)
        self.assertEqual(ks3, 0x44D09670, 'ks3 assert failed')
        atEnc = at ^ ks3
        self.assertEqual(atEnc, 0xD9A573DD, 'atEnc assert failed')

    def test_mfkey32_accepts_the_known_key_and_rejects_a_wrong_key(self):
        self.assertTrue(Crypto1.mfkey32_is_reader_has_key(
            uid=0x3935CC65,
            nt=0xCE0A3795,
            nrEnc=0xACF77858,
            arEnc=0x8EB38AD5,
            key='9FFFD8B16709'
        ))

        self.assertFalse(Crypto1.mfkey32_is_reader_has_key(
            uid=0x3935CC65,
            nt=0xCE0A3795,
            nrEnc=0xACF77858,
            arEnc=0x8EB38AD5,
            key='FFFFFFFFFFFF'
        ))


if __name__ == '__main__':
    unittest.main()
