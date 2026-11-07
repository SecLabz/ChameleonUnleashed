#!/usr/bin/env python3
"""Prepare a standalone HTTPS/loopback BLE app; does not publish anything."""
import argparse
from pathlib import Path
import tempfile

from prepare_pages import cdn_files
from hosted_web import FILES, shell

ROOT = Path(__file__).resolve().parents[2]


def prepare(site, resources, output):
    if output.exists() and any(output.iterdir()):
        raise ValueError('Hosted output directory must be empty')
    version, files = shell(site)
    cdn = cdn_files(resources.read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.hosted-') as temporary:
        staging = Path(temporary) / 'site'
        app = staging / 'app'
        app.mkdir(parents=True)
        for name, data in files.items():
            (app / name).write_bytes(data)
        for name, data in cdn.items():
            path = staging / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (staging / '.nojekyll').write_bytes(b'')
        if output.exists():
            output.rmdir()
        staging.replace(output)
    return version


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--site', type=Path, default=ROOT / '.cache/web/assets/site')
    parser.add_argument('--resources', type=Path, default=ROOT / '.cache/web/chameleon-unleashed-cdn.zip')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print('Hosted app:', prepare(args.site, args.resources, args.output), 'at', args.output / 'app')
