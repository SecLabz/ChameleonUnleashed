#!/usr/bin/env python3
"""Package published platform downloads and standalone firmware/resources."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from project_version import check_tag, read_version

PLATFORMS = ('linux-x64', 'macos-arm64', 'macos-x64', 'windows-x64')
FIRMWARE = {
    'installation': 'chameleon-unleashed-install.zip',
    'bootloader': 'chameleon-unleashed-bootloader.zip',
    'application': 'chameleon-unleashed-app.zip',
    'system': 'chameleon-unleashed-full.zip',
    'images': 'chameleon-unleashed-binaries.zip',
    'cdn': 'chameleon-unleashed-cdn.zip',
}


def read_zip(path):
    files = {}
    with ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise ValueError(f'Corrupt archive: {path}')
        for entry in archive.infolist():
            name = entry.filename
            parts = PurePosixPath(name)
            if parts.is_absolute() or '..' in parts.parts or '\\' in name or ':' in name:
                raise ValueError(f'Unsafe archive path: {name}')
            if stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError(f'Archive symlink is not supported: {name}')
            if entry.is_dir():
                continue
            if name in files:
                raise ValueError(f'Duplicate archive entry: {name}')
            files[name] = archive.read(entry)
    return files


def write_zip(path, files, executable=None):
    with ZipFile(path, 'w', compression=ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            entry = ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = (stat.S_IFREG | (0o755 if name == executable else 0o644)) << 16
            archive.writestr(entry, data)


def installation(label, platform):
    windows = platform == 'windows-x64'
    terminal = 'PowerShell' if windows else 'a terminal'
    command = '   .\\chameleon.exe flash firmware.zip' if windows else (
        '   chmod +x chameleon\n   ./chameleon flash firmware.zip')
    text = f"""ChameleonUnleashed {label}

1. Connect one Chameleon Ultra using a USB data cable.
   Close any other application using the device.
   Before the first bootloader migration, export your saved tags:
   an older bootloader may overwrite slot storage during migration.

2. Open {terminal} in this folder and run:

{command}

3. Confirm the update and keep the device connected
   until flashing completes and the Ultra restarts.

Keep firmware.zip intact - do not extract it.
Use the same command for installation, reinstallation and future updates.
First installation requires S140 7.2.0.

You can also update through a browser on a compatible device connected
to the Ultra over USB. Open http://192.168.16.1 and choose
Device > Update firmware.

Help:
https://github.com/SecLabz/ChameleonUnleashed#quick-installation
"""
    return text.replace('\n', '\r\n').encode() if windows else text.encode()


def prepare(artifacts, output, version, tag=None, development=False, firmware_only=False):
    check_tag(version, tag)
    if tag and development:
        raise ValueError('A tagged release cannot be a development snapshot')
    if output.exists() and any(output.iterdir()):
        raise ValueError('Output directory must be empty')

    sources = {key: artifacts / name for key, name in FIRMWARE.items()}
    firmware = {key: read_zip(path) for key, path in sources.items()}
    manifests = [name for name in firmware['cdn'] if name.startswith('releases/') and name.endswith('.json')]
    if len(manifests) != 1:
        raise ValueError('Expected one matching CDN release manifest')
    manifest = json.loads(firmware['cdn'][manifests[0]])
    if manifest.get('projectVersion') != version:
        raise ValueError('CDN project version differs from the release version')
    for resource in manifest['resources']:
        data = firmware['cdn'][resource['path']]
        if len(data) != resource['size'] or hashlib.sha256(data).hexdigest() != resource['sha256']:
            raise ValueError(f"CDN resource differs from its manifest: {resource['path']}")

    label = 'v' + version
    if development:
        revision = manifest['source']['revision']
        if not re.fullmatch('[0-9a-f]{40}', revision or ''):
            raise ValueError('Development downloads require a source commit')
        label += '-dev.' + revision[:7]

    application = sources['application'].read_bytes()
    installation_package = sources['installation'].read_bytes()
    downloads = {}
    for platform in (() if firmware_only else PLATFORMS):
        native = read_zip(artifacts / f'chameleon-{platform}.zip')
        executable = 'chameleon.exe' if platform == 'windows-x64' else 'chameleon'
        if set(native) != {executable}:
            raise ValueError(f'Unexpected native archive contents for {platform}')
        downloads[f'ChameleonUnleashed-{label}-{platform}.zip'] = (
            {executable: native[executable], 'firmware.zip': installation_package,
             'INSTALL.txt': installation(label, platform)}, executable)

    resources = {
        'firmware/install.zip': installation_package,
        'firmware/bootloader.zip': sources['bootloader'].read_bytes(),
        'firmware/application.zip': application,
        'firmware/system.zip': sources['system'].read_bytes(),
        **{'images/' + name: data for name, data in firmware['images'].items()},
        **{'cdn/' + name: data for name, data in firmware['cdn'].items()},
        'README.txt': f"""ChameleonUnleashed {label} - firmware & web resources

firmware/install.zip      Install, reinstall or update through the CLI.
firmware/bootloader.zip   Separate bootloader for DFU clients that need it.
firmware/application.zip  Update a device that already has web recovery.
firmware/system.zip       Firmware with bootloader and SoftDevice.
images/                   HEX images for direct programming.
cdn/                      Web resources for self-hosting.

You can also update in a browser on a compatible USB-connected device:
http://192.168.16.1 > Device > Update firmware.

Keep the DFU ZIPs intact. Export saved tags before the first bootloader
migration, system updates or switching firmware. First installation requires S140 7.2.0.

For self-hosting, publish cdn/ at the firmware's configured resource URL.
Use matching firmware and resources, and keep older resources available.

Guide: https://github.com/SecLabz/ChameleonUnleashed/blob/main/docs/web-updates.md
""".encode(),
    }
    downloads[f'ChameleonUnleashed-{label}-firmware-resources.zip'] = (resources, None)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.release-') as temporary:
        staging = Path(temporary)
        for name, (files, executable) in downloads.items():
            write_zip(staging / name, files, executable)
        output.mkdir(exist_ok=True)
        for name in downloads:
            (staging / name).replace(output / name)
    return sorted(downloads)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tag')
    parser.add_argument('--version', help='Version of verified CI source artifacts; defaults to the workspace')
    parser.add_argument('--development', action='store_true')
    args = parser.parse_args()
    for name in prepare(args.artifacts, args.output, args.version or read_version(), args.tag, args.development):
        print(args.output / name)


if __name__ == '__main__':
    main()
