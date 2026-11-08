#!/usr/bin/env python3
"""Mirror signed upstream ZIPs unchanged for the embedded browser (GitHub asset CORS)."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
from functools import lru_cache
import tempfile
from urllib.request import Request, urlopen
from zipfile import ZipFile, BadZipFile
import zlib
from cryptography.exceptions import InvalidSignature

from nordic_update import inspect_zip, MAX_ZIP
from web_update import (RECOVERY_START, APPLICATION_START, APPLICATION_END,
                        RECOVERY_DESCRIPTOR, RECOVERY_MAGIC, RECOVERY_FOOTER_OFFSET, LAYOUT,
                        BOOTLOADER_START, BOOTLOADER_END, SYSTEM_STAGE, SOFTDEVICE_START,
                        IMAGE_VECTOR_SIZE, FLASH_ERASED_BYTE)

SOURCES = (('unleashed', 'Unleashed', 'SecLabz/ChameleonUnleashed'),
           ('official', 'Official', 'RfidResearchGroup/ChameleonUltra'))
MAX_DOWNLOAD = 128 * 1024 * 1024
RELEASE_LIMIT = 5
PAGE_SIZE = 100
MAX_RELEASE_PAGES = 100
MAX_RELEASE_RESPONSE = 8 * 1024 * 1024
DOWNLOAD_TIMEOUT_SECONDS = 30


def fetch(url, maximum, api=False):
    if not url.startswith(('https://api.github.com/repos/', 'https://github.com/')):
        raise ValueError('Unexpected upstream URL')
    headers = {'User-Agent': 'ChameleonUnleashed-firmware-mirror', 'Accept': 'application/vnd.github+json' if api else 'application/octet-stream'}
    token = os.environ.get('GITHUB_TOKEN')
    if api and token:
        headers['Authorization'] = 'Bearer ' + token
    with urlopen(Request(url, headers=headers), timeout=DOWNLOAD_TIMEOUT_SECONDS) as response:
        if not response.url.startswith(('https://api.github.com/', 'https://github.com/', 'https://release-assets.githubusercontent.com/')):
            raise ValueError('Unexpected upstream redirect')
        data = response.read(maximum + 1)
    if len(data) > maximum:
        raise ValueError('Upstream response exceeds download limit')
    return data


def save_atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
        temporary.write(data)
        name = Path(temporary.name)
    try:
        name.chmod(0o644)
        name.replace(path)
    finally:
        name.unlink(missing_ok=True)


def verified_asset(repo, asset, get=fetch):
    url = asset['browser_download_url']
    if not url.startswith('https://github.com/' + repo + '/releases/download/') or not 0 < asset['size'] <= MAX_DOWNLOAD:
        raise ValueError('Unexpected release asset')
    data = get(url, MAX_DOWNLOAD)
    if len(data) != asset['size']:
        raise ValueError('Truncated release asset')
    digest = asset.get('digest')
    if digest and digest != 'sha256:' + hashlib.sha256(data).hexdigest():
        raise ValueError('Release asset differs from GitHub digest')
    return data


def application_zip(bundle):
    """Extract original DFU bytes from the existing five-download release layout."""
    with ZipFile(io.BytesIO(bundle)) as archive:
        if len(set(archive.namelist())) != len(archive.infolist()):
            raise ValueError('Duplicate resources archive entries')
        entry = archive.getinfo('firmware/application.zip')
        if entry.file_size > MAX_ZIP:
            raise ValueError('Nested firmware ZIP is too large')
        return archive.read(entry)


def releases(repo, get=fetch):
    """GitHub orders by creation time; catalogue order follows publication time."""
    result = []
    for page in range(1, MAX_RELEASE_PAGES + 1):
        batch = json.loads(get(f'https://api.github.com/repos/{repo}/releases?per_page={PAGE_SIZE}&page={page}',
                               MAX_RELEASE_RESPONSE, api=True))
        if not isinstance(batch, list) or any(not isinstance(item, dict) for item in batch):
            raise ValueError('Expected a GitHub release list')
        result.extend(item for item in batch if not item.get('draft'))
        if len(batch) < PAGE_SIZE:
            return sorted(result, key=lambda item: item.get('published_at') or '', reverse=True)
    raise ValueError('Release history exceeded the pagination limit')


@lru_cache(maxsize=1)
def softdevice():
    from nordic_update import ROOT
    from web_update import read_hex
    memory = read_hex(ROOT / 'firmware/nrf52_sdk/components/softdevice/s140/hex/s140_nrf52_7.2.0_softdevice.hex')
    return bytes(memory.get(address, FLASH_ERASED_BYTE) for address in range(SOFTDEVICE_START, RECOVERY_START))


def catalogue_package(source_id, data):
    if source_id == 'unleashed':
        images = inspect_zip(data)
        app = images['application']
        payload = app['payload']
        # Older Unleashed releases remain on GitHub, but do not offer permanent
        # web recovery. Their complete restoration stays a local-file/DFU path.
        if (set(images) != {'application'}
                or not APPLICATION_START - RECOVERY_START + IMAGE_VECTOR_SIZE <= len(payload) <= APPLICATION_END - RECOVERY_START
                or payload[RECOVERY_FOOTER_OFFSET:RECOVERY_FOOTER_OFFSET + len(RECOVERY_MAGIC)] != RECOVERY_MAGIC):
            return None
        _, layout, checksum = RECOVERY_DESCRIPTOR.unpack_from(payload, RECOVERY_FOOTER_OFFSET)
        if layout != LAYOUT or checksum != zlib.crc32(payload[:RECOVERY_FOOTER_OFFSET]):
            raise ValueError('Invalid recovery layout footer')
        return LAYOUT, 'application', data

    images = inspect_zip(data)
    app = images['application']
    system = images.get('softdevice_bootloader', images.get('bootloader'))
    if (len(images) != 2 or not system
            or not IMAGE_VECTOR_SIZE <= app['app_size'] <= SYSTEM_STAGE - RECOVERY_START
            or not IMAGE_VECTOR_SIZE <= system['bl_size'] <= BOOTLOADER_END - BOOTLOADER_START
            or system['sd_size'] > RECOVERY_START - SOFTDEVICE_START
            or app['payload'][RECOVERY_FOOTER_OFFSET:RECOVERY_FOOTER_OFFSET + len(RECOVERY_MAGIC)] == RECOVERY_MAGIC):
        return None
    size = system['sd_size']
    if system['payload'][:size] != softdevice()[:size]:
        raise ValueError('Official package changes the installed S140 7.2.0 bytes')
    return 0, 'full', data


def mirror(destination, limit=RELEASE_LIMIT, get=fetch, on_resources=None):
    if not 1 <= limit <= RELEASE_LIMIT:
        raise ValueError('Catalogue limit must be between 1 and 5')
    sources = []
    for source_id, label, repo in SOURCES:
        rows = []
        for release in releases(repo, get):
            assets = release.get('assets', [])
            if not assets:
                continue
            eligible = (not release.get('prerelease') and release.get('tag_name') != 'dev'
                        and len(rows) < limit)
            if source_id == 'unleashed':
                if not eligible and on_resources is None:
                    continue
                asset = next((asset for asset in sorted(assets, key=lambda a: a.get('created_at', ''), reverse=True) if re.fullmatch(
                    r'ChameleonUnleashed-.+-firmware-resources\.zip', asset['name'])), None)
            else:
                if not eligible:
                    continue
                asset = next((asset for asset in assets if asset['name'] == 'ultra-dfu-full.zip'), None)
            if asset is None:
                continue
            # Transport failures and GitHub digest mismatches abort preparation,
            # rather than replacing a working catalogue with a truncated list.
            data = verified_asset(repo, asset, get)
            if source_id == 'unleashed' and on_resources is not None:
                on_resources(data, release)
            if not eligible:
                continue
            try:
                selected = catalogue_package(source_id, application_zip(data) if source_id == 'unleashed' else data)
                if selected is None:
                    continue
                layout, kind, data = selected
                identity = hashlib.sha256(data).hexdigest()
                path = 'files/' + identity + '.zip'
                save_atomic(destination / path, data)
                downloads = {kind: {'path': path, 'size': len(data), 'sha256': identity}}
                rows.append({'version': release['tag_name'], 'prerelease': False,
                             'published_at': release.get('published_at'), 'url': release['html_url'],
                             'layout': layout, 'packages': downloads})
            except (ValueError, KeyError, InvalidSignature, BadZipFile) as error:
                print(f"Skipping {repo} {release.get('tag_name')}: {str(error) or type(error).__name__}")
            if len(rows) >= limit and (on_resources is None or source_id == 'official'):
                break
        sources.append({'id': source_id, 'label': label, 'repository': repo, 'releases': rows})
    document = {'format': 1, 'generated_at': datetime.now(timezone.utc).isoformat(), 'sources': sources}
    save_atomic(destination / 'catalog.json', (json.dumps(document, indent=2) + '\n').encode())
    return document


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='The static updates/ directory')
    parser.add_argument('--limit', type=int, default=RELEASE_LIMIT)
    options = parser.parse_args()
    if not 1 <= options.limit <= RELEASE_LIMIT:
        parser.error('--limit must be between 1 and 5')
    catalogue = mirror(options.output, options.limit)
    for source in catalogue['sources']:
        print(source['repository'], len(source['releases']), 'verified releases')
