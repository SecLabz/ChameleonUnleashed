#!/usr/bin/env python3
"""Verify uploaded release archives before removing superseded download names."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from package_release import PLATFORMS


def superseded(assets, archives):
    if len(archives) != len(PLATFORMS) + 1:
        raise ValueError('Expected all five packaged release archives')
    uploaded = {asset['name']: asset for asset in assets}
    for name, data in archives.items():
        asset = uploaded.get(name, {})
        if (asset.get('state') != 'uploaded' or asset.get('size') != len(data)
                or asset.get('digest') != 'sha256:' + hashlib.sha256(data).hexdigest()):
            raise ValueError(f'Uploaded archive is missing or differs: {name}')
    return [asset for asset in assets if asset['name'] not in archives]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'))
    parser.add_argument('--tag', required=True)
    parser.add_argument('--archives', type=Path, required=True)
    parser.add_argument('--development', action='store_true')
    args = parser.parse_args()
    if not args.repo:
        parser.error('--repo or GITHUB_REPOSITORY is required')
    archives = {path.name: path.read_bytes() for path in args.archives.glob('*.zip')}
    release = json.loads(subprocess.check_output([
        'gh', 'api', f'repos/{args.repo}/releases/tags/{args.tag}']))
    if args.development and args.tag != 'dev':
        parser.error('Development cleanup is restricted to the dev pre-release')
    if args.development and (not release.get('prerelease') or release.get('draft')):
        raise ValueError('Expected a public development pre-release')
    for asset in superseded(release['assets'], archives):
        subprocess.run(['gh', 'api', '--method', 'DELETE',
                        f"repos/{args.repo}/releases/assets/{asset['id']}"], check=True)
        print('Removed superseded download:', asset['name'])


if __name__ == '__main__':
    main()
