#!/usr/bin/env python3
"""Encode Unleashed's DFU version; increment the revision when the bootloader changes."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FAMILY = b'CU'
MAX_REVISION = (1 << 16) - 1


def version_number(revision):
    if type(revision) is not int or not 1 <= revision <= MAX_REVISION:
        raise ValueError('Bootloader revision must be an integer between 1 and 65535')
    return int.from_bytes(FAMILY + revision.to_bytes(2, 'big'), 'big')


def revision_number(version):
    if type(version) is not int or not 0 <= version < (1 << 32):
        return None
    data = version.to_bytes(4, 'big')
    revision = int.from_bytes(data[2:], 'big')
    return revision if data[:2] == FAMILY and revision else None


def read_version(root=ROOT):
    return version_number(json.loads((root / 'firmware/bootloader/version.json').read_text())['revision'])


def check_transition(current, previous=None):
    version = current['version']
    if revision_number(version) is None:
        raise ValueError('New bootloader builds require an Unleashed revision')
    if previous is None or revision_number(previous['version']) is None:
        return
    if version < previous['version']:
        raise ValueError('Published bootloader revisions must not go backwards')
    if version == previous['version'] and current['payload'] != previous['payload']:
        raise ValueError('Bootloader changed: increment firmware/bootloader/version.json')


if __name__ == '__main__':
    print(read_version())
