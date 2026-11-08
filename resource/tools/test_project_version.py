"""Project versions fit the legacy USB reply and release tags match their build."""

from pathlib import Path
import tempfile
import unittest

from project_version import check_tag, read_version, parse_version


class ProjectVersionTests(unittest.TestCase):
    def test_workspace_is_the_version_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'software').mkdir()
            manifest = root / 'software/Cargo.toml'
            for version in ['0.1.0', '1.0.0', '3.7.12', '255.255.0']:
                manifest.write_text(f'[workspace.package]\nversion = "{version}"\n[workspace.dependencies]\nother = "9.9.9"\n')
                self.assertEqual(read_version(root), version)
            for version in ['1.0', '01.0.0', '256.0.0', '1.256.0', '1.0.65536', 'invalid']:
                manifest.write_text(f'[workspace.package]\nversion = "{version}"\n')
                with self.assertRaises(ValueError):
                    read_version(root)

    def test_release_tag_must_match_the_workspace(self):
        check_tag('1.0.0', None)
        check_tag('1.0.0', '')
        check_tag('1.0.0', 'v1.0.0')
        for tag in ['v0.1.0', 'v1.0', 'v2.0.0']:
            with self.assertRaises(ValueError):
                check_tag('1.0.0', tag)

    def test_tagged_manifest_can_be_checked_independently_of_a_newer_workspace(self):
        tagged = '[workspace.package]\nversion = "1.2.3"\n'
        self.assertEqual(parse_version(tagged), '1.2.3')
        check_tag(parse_version(tagged), 'v1.2.3')
        for version in ['../outside', '1.0.65536', '01.0.0']:
            with self.assertRaises(ValueError):
                check_tag(version, 'v' + version)


if __name__ == '__main__':
    unittest.main()
