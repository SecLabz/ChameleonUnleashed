"""Embed the self-contained recovery page, also used by the main application."""
import gzip
import json
from pathlib import Path
import sys

from build_web import resource_origin
from web_update import ROOT


def assets(base):
    files = {name: (ROOT / 'software/web/public' / name).read_bytes()
             for name in ('update.html', 'update.css')}
    files['nordic-package.mjs'] = (ROOT / 'software/web/src/ui/nordic-package.mjs').read_bytes()
    files['nordic-package.mjs'] = files['nordic-package.mjs'].replace(b'../vendor/unzip.mjs', b'./update-unzip.mjs').replace(b'../runtime/sha256.mjs', b'./update-sha256.mjs')
    files['firmware-catalogue.mjs'] = (ROOT / 'software/web/src/ui/firmware-catalogue.mjs').read_bytes().replace(b'../runtime/sha256.mjs', b'./update-sha256.mjs')
    files['update-unzip.mjs'] = (ROOT / 'software/web/src/vendor/unzip.mjs').read_bytes()
    files['fflate.LICENSE.txt'] = (ROOT / 'software/web/src/vendor/fflate.LICENSE.txt').read_bytes()
    files['update.mjs'] = (ROOT / 'software/web/src/ui/firmware-update.mjs').read_bytes()
    files['update-config.mjs'] = ('export const RESOURCE_BASE = ' + json.dumps(base) + ';\n').encode()
    files['update-sha256.mjs'] = (ROOT / 'software/web/src/runtime/sha256.mjs').read_bytes()
    return files


def prepare(base, destination):
    origin = resource_origin(base)
    destination.mkdir(parents=True, exist_ok=True)
    lines = ['#pragma once', '#include <stdint.h>', '#include <string.h>',
             '#define WEB_RESOURCE_CONNECT ' + json.dumps("'self'" + (' ' + origin if origin else '')),
             'typedef struct { const char *path, *type; const uint8_t *bytes; size_t size; } web_asset_t;']
    rows = []
    types = {'.html': 'text/html; charset=utf-8', '.mjs': 'text/javascript', '.css': 'text/css', '.txt': 'text/plain; charset=utf-8'}
    for index, (name, data) in enumerate(sorted(assets(base).items())):
        packed = gzip.compress(data, compresslevel=9, mtime=0)
        lines.append(f'static const uint8_t recovery_asset_{index}[] = {{' + ','.join(map(str, packed)) + '};')
        rows.append('{' + json.dumps('/' + name) + ',' + json.dumps(types[Path(name).suffix]) +
                    f',recovery_asset_{index},sizeof(recovery_asset_{index})' + '}')
    lines += ['static const web_asset_t recovery_assets[] = {' + ','.join(rows) + '};',
              'static const web_asset_t *web_asset_find(const char *path) {',
              ' if (!strcmp(path,"/") || !strcmp(path,"/update")) path="/update.html";',
              ' for (size_t i=0;i<sizeof(recovery_assets)/sizeof(*recovery_assets);i++)',
              '  if (!strcmp(path,recovery_assets[i].path)) return &recovery_assets[i];',
              ' return NULL;', '}']
    (destination / 'web_assets.h').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    config = json.loads((ROOT / '.cache/web/assets/config.json').read_text())
    prepare(config['baseUrl'], Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / '.cache/web/recovery')
