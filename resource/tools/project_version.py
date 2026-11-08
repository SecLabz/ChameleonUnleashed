#!/usr/bin/env python3
"""Read the shared project version from the Rust workspace; optionally check a release tag."""

import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def validate_version(version):
    match = re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version)
    if not match or any(int(part) > 255 for part in match.groups()[:2]) or int(match[3]) > 65535:
        raise ValueError('Workspace version must be MAJOR.MINOR.PATCH with 8-bit major/minor and a 16-bit patch')
    return version


def parse_version(manifest):
    section = re.search(r'(?ms)^\[workspace\.package\]\s*\n(.*?)(?=^\[|\Z)', manifest)
    entry = re.search(r'^version\s*=\s*"([^"]+)"\s*(?:#.*)?$', section[1], re.M) if section else None
    return validate_version(entry[1] if entry else '')


def read_version(root=ROOT):
    return parse_version((root / 'software/Cargo.toml').read_text())


def check_tag(version, tag):
    validate_version(version)
    if tag and tag != 'v' + version:
        raise ValueError(f'Release tag {tag!r} must match workspace version v{version}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', help='Require this release tag to match the project version')
    parser.add_argument('--dfu', action='store_true', help='Print the numeric firmware version (8/8/16 bits)')
    args = parser.parse_args()
    try:
        version = read_version()
        check_tag(version, args.tag)
    except ValueError as error:
        parser.error(str(error))
    if args.dfu:
        from web_update import version_number
        print(version_number(version))
    else:
        print(version)


if __name__ == '__main__':
    main()
