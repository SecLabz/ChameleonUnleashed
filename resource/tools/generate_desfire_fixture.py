"""Build the public DESFire test image from synthetic data, without a card dump.

The fixed seed keeps native, Rust and browser expectations reproducible.
Run with --check to detect an accidental replacement by a physical capture.
"""

import argparse
import json
from pathlib import Path
import random

FIXTURE = Path(__file__).with_name('tests') / 'desfire-public.json'


def fixture():
    rng = random.Random(0xD35F1A7E)

    def octets(count):
        return [rng.randrange(256) for _ in range(count)]

    def file(file_id, settings, data, state=0):
        return ([file_id, 0, *len(settings).to_bytes(2, 'little'), *settings, state,
                 *len(data).to_bytes(4, 'little'), *data])

    # Manufacturer/product/protocol fields describe a test DESFire card.
    # UID, production batch, AID and contents are generated independently.
    uid = [4, *octets(6)]
    ats = [6, 0x75, 0x77, 0x81, 0, 0x80]
    hardware = [4, 1, 1, 0x12, 0, 0x18, 5]
    version = [*hardware, *hardware, *uid, *octets(5), 0x01, 0x25]
    aid = octets(3)
    metadata = [
        *b'DFA1', 1, 0x44, 3, 0x20, len(ats), *ats, *version,
        1, *int(1536).to_bytes(4, 'little'), 0, 2, 15, 0x81, 1, 0, 0,
    ]
    memory = [
        *b'DFD1', 1, 1, *aid, 0, 2, 15, 0x81, 1, 1, 0, 3,
        # Public 64-byte file, denied backup file, three two-byte records.
        *file(1, [0, 0, 0xe0, 0xee, 64, 0, 0], octets(64)),
        *file(2, [1, 0, 0, 0, 32, 0, 0], [], state=1),
        *file(3, [3, 0, 0xe0, 0xee, 2, 0, 0, 4, 0, 0, 3, 0, 0], octets(6)),
    ]
    return dict(version=1, protocol='desfire', uid=uid, metadata=metadata,
                memory=memory, complete=True, error=None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    expected = json.dumps(fixture(), indent=2) + '\n'
    if args.check:
        if FIXTURE.read_text() != expected:
            raise SystemExit('DESFire fixture differs from the synthetic generator')
    else:
        FIXTURE.write_text(expected)


if __name__ == '__main__':
    main()
