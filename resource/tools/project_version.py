#!/usr/bin/env python3
"""Read the shared project version from the Rust workspace; optionally check a release tag."""

import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read_version(root=ROOT):
    manifest = (root / 'software/Cargo.toml').read_text()
    section = re.search(r'(?ms)^\[workspace\.package\]\s*\n(.*?)(?=^\[|\Z)', manifest)
    entry = re.search(r'^version\s*=\s*"([^"]+)"\s*(?:#.*)?$', section[1], re.M) if section else None
    version = entry[1] if entry else ''
    match = re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version)
    if not match or any(int(part) > 255 for part in match.groups()[:2]):
        raise ValueError('Workspace version must be MAJOR.MINOR.PATCH with major/minor fitting the USB version bytes')
    return version


def check_tag(version, tag):
    if tag and tag != 'v' + version:
        raise ValueError(f'Release tag {tag!r} must match workspace version v{version}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', help='Require this release tag to match the project version')
    args = parser.parse_args()
    try:
        version = read_version()
        check_tag(version, args.tag)
    except ValueError as error:
        parser.error(str(error))
    print(version)


if __name__ == '__main__':
    main()
