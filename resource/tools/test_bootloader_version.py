"""Bootloader identity and the version-only skip contract."""
import unittest

from bootloader_version import check_transition, read_version, revision_number, version_number


class BootloaderVersionTests(unittest.TestCase):
    def test_family_distinguishes_our_revisions_from_legacy_counters(self):
        self.assertIsNotNone(revision_number(read_version()))
        for revision in (1, 5, 65535):
            self.assertEqual(revision_number(version_number(revision)), revision)
        for version in (0, 1, 3, 5, -1, (1 << 32), int.from_bytes(b'XX\0\x01', 'big')):
            self.assertIsNone(revision_number(version))
        for revision in (0, -1, 65536, True, '1'):
            with self.assertRaises(ValueError):
                version_number(revision)

    def test_only_unchanged_bytes_can_retain_a_published_revision(self):
        first = {'version': version_number(1), 'payload': b'first'}
        check_transition(first)
        check_transition(first, {'version': 5, 'payload': b'legacy'})
        check_transition(first, first)
        with self.assertRaisesRegex(ValueError, 'increment'):
            check_transition({**first, 'payload': b'changed'}, first)
        second = {'version': version_number(2), 'payload': b'changed'}
        check_transition(second, first)
        with self.assertRaisesRegex(ValueError, 'backwards'):
            check_transition(first, second)
        with self.assertRaisesRegex(ValueError, 'require an Unleashed revision'):
            check_transition({'version': 5, 'payload': b'legacy'})
