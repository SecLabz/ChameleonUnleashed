"""Missing-swap repair must never erase records or unknown page contents."""
from pathlib import Path
import re
import unittest

from native_test import function, run_c

ROOT = Path(__file__).resolve().parents[2]


class FdsRecoveryTests(unittest.TestCase):
    def test_empty_page_only_and_flash_failures(self):
        source = (ROOT / 'firmware/application/src/utils/fds_util.c').read_text()
        fixture = (ROOT / 'resource/tools/tests/fds_recovery.c').read_text()
        definitions = (ROOT / 'firmware/nrf52_sdk/components/libraries/fds/fds_internal_defs.h').read_text()
        constants = '\n'.join(re.findall(r'^#define FDS_(?:PAGE_TAG_\w+|ERASED_WORD)\s+.*$', definitions, re.M))
        fixture = fixture.replace('/* SDK_PAGE_CONSTANTS */', constants)
        fixture = fixture.replace('/* PRODUCTION_REPAIR */', function(source, 'fds_recover_empty_swap'))
        run_c(fixture.replace('/* PRODUCTION_INIT */', function(source, 'fds_util_init')))


if __name__ == '__main__':
    unittest.main()
