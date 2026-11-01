"""Verify native esbuild platform selection, integrity and offline cache repair."""
import io
import pathlib
import tempfile
import unittest
from unittest import mock

import esbuild_tool


class EsbuildToolTests(unittest.TestCase):
    def test_platforms_and_cache(self):
        import base64
        import hashlib
        import json
        import tarfile

        for system, machine, expected in [
            ("Darwin", "arm64", "darwin-arm64"),
            ("Darwin", "x86_64", "darwin-x64"),
            ("Linux", "x86_64", "linux-x64"),
            ("Linux", "aarch64", "linux-arm64"),
            ("Linux", "armv7l", "linux-arm"),
            ("Windows", "AMD64", "win32-x64"),
            ("Windows", "ARM64", "win32-arm64"),
        ]:
            with mock.patch.object(esbuild_tool.platform, "system", return_value=system), \
                 mock.patch.object(esbuild_tool.platform, "machine", return_value=machine):
                assert esbuild_tool.platform_key() == expected
                assert expected in json.loads(esbuild_tool.CONFIG.read_text())["integrity"]

        executable = b"test-native-executable"
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
            member = tarfile.TarInfo("package/bin/esbuild")
            member.size = len(executable)
            archive.addfile(member, io.BytesIO(executable))
        payload = buffer.getvalue()
        integrity = "sha512-" + base64.b64encode(hashlib.sha512(payload).digest()).decode()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config = root / "esbuild.json"
            config.write_text(json.dumps({"version": "test", "integrity": {"linux-x64": integrity}}))
            with mock.patch.object(esbuild_tool, "CONFIG", config), \
                 mock.patch.object(esbuild_tool, "CACHE", root / "cache"), \
                 mock.patch.object(esbuild_tool, "platform_key", return_value="linux-x64"), \
                 mock.patch.object(esbuild_tool, "urlopen", return_value=io.BytesIO(payload)) as download:
                binary = esbuild_tool.ensure_esbuild()
                assert binary.read_bytes() == executable
                download.assert_called_once()
                download.side_effect = AssertionError("cached builds must work offline")
                assert esbuild_tool.ensure_esbuild() == binary
                binary.write_bytes(b"corrupted")
                assert esbuild_tool.ensure_esbuild().read_bytes() == executable
                (binary.parent / "package.tgz").write_bytes(b"corrupted archive")
                try:
                    esbuild_tool.ensure_esbuild()
                except RuntimeError as error:
                    assert "integrity mismatch" in str(error)
                else:
                    raise AssertionError("corrupted cache accepted")


if __name__ == "__main__":
    unittest.main()
