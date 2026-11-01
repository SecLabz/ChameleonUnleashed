"""Exercise both production timeslot observers with broadcast SoftDevice events."""
from pathlib import Path
import re
import unittest

from native_test import run_c

ROOT = Path(__file__).resolve().parents[2]


class TimeslotTests(unittest.TestCase):
    def test_session_ownership_and_legacy_retries(self):
        sources = []
        for name in ('timeslot.c', 'rf_timeslot.c'):
            source = (ROOT / 'firmware/application/src/utils' / name).read_text()
            sources.append(re.sub(r'^#include.*$', '', source, flags=re.M))
        fixture = (ROOT / 'resource/tools/tests/timeslots.c').read_text()
        run_c(fixture.replace('/* TIMESLOT IMPLEMENTATIONS */', '\n'.join(sources)))


if __name__ == '__main__':
    unittest.main()
