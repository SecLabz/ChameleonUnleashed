"""Build consumers use the package's declared files and shared release version."""
import json
from pathlib import Path
import tempfile
import unittest

from build_web import wasm_package


class WasmPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'Cargo.toml').write_text(
            '[workspace]\nmembers = ["crate"]\nresolver = "3"\n'
            '[workspace.package]\nversion = "0.1.0"\n'
        )
        crate = self.root / 'crate'
        (crate / 'src').mkdir(parents=True)
        (crate / 'Cargo.toml').write_text(
            '[package]\nname = "nfc-wasm"\nversion.workspace = true\n'
        )
        (crate / 'src/lib.rs').write_text('')
        self.directory = self.root / 'packages/nfc-wasm'
        self.directory.mkdir(parents=True)
        self.package = {
            'version': '0.1.0',
            'exports': {'./worker': './worker.mjs'},
            'files': ['index.mjs', 'engine.mjs', 'worker.mjs', 'LICENSE']
        }
        for name in [*self.package['files'], 'test.mjs']:
            (self.directory / name).write_text(name)

    def read_package(self):
        (self.directory / 'package.json').write_text(json.dumps(self.package))
        return wasm_package(self.root)

    def test_only_declared_runtime_modules_are_embedded(self):
        modules, worker = self.read_package()
        self.assertEqual(worker, 'worker.mjs')
        self.assertEqual(modules, {
            name: name.encode() for name in ['index.mjs', 'engine.mjs', 'worker.mjs']
        })

    def test_mismatched_releases_fail_before_assembly(self):
        self.package['version'] = '0.2.0'
        with self.assertRaisesRegex(SystemExit, 'versions differ'):
            self.read_package()

    def test_worker_must_be_part_of_the_distribution(self):
        self.package['files'].remove('worker.mjs')
        with self.assertRaisesRegex(ValueError, 'Worker entry point'):
            self.read_package()


if __name__ == '__main__':
    unittest.main()
