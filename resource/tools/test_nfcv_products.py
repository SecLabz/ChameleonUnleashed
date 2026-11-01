"""Focused ISO15693 SLIX and SLIX-L image and command checks."""

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/application/src/rfid"


class NfcvProductsTest(unittest.TestCase):
    def test_products(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "nfcv_products"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(SOURCE),
                str(ROOT / "resource/tools/tests/nfcv_products.c"),
                *(str(SOURCE / "iso15693" / f"{name}.c") for name in
                  ("frame", "memory", "standard", "slix", "session")),
                "-o", str(target),
            ], check=True)
            subprocess.run([str(target)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
