"""Recover the explicitly identified PM3 after physical USB reconnection.

Usage: python3 pm3-recovery.py MANIFEST.json OUTPUT_DIRECTORY
No simulation or flash command is issued. A missing port, mismatched backup,
unconfirmed ping, changed Ultra or remaining NFC-A identity cannot report ready.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import urllib.request


def http_request(url, body=None):
    request = urllib.request.Request(url, data=body,
                                     headers={'Content-Type': 'application/octet-stream'})
    with urllib.request.urlopen(request, timeout=10) as response:
        data = response.read()
        return json.loads(data) if 'json' in response.headers.get('Content-Type', '') else data


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def recover(manifest_path, destination, *, http=http_request, execute=subprocess.run):
    manifest_path = Path(manifest_path).resolve()
    manifest = json.loads(manifest_path.read_text())
    required = ('pm3Client', 'pm3Port', 'ramBackup', 'ramSha256', 'ultraURL', 'ultraChip')
    if manifest.get('version') != 1 or any(not isinstance(manifest.get(key), str) or not manifest[key] for key in required):
        raise ValueError('Invalid recovery manifest')
    if any(not Path(manifest[key]).is_absolute() for key in ('pm3Client', 'pm3Port', 'ramBackup')):
        raise ValueError('Recovery manifest paths must be absolute')
    backup = Path(manifest['ramBackup']).read_bytes()
    if len(backup) != 4096 or sha256(backup) != manifest['ramSha256']:
        raise ValueError('RAM backup size/hash does not match the frozen recovery manifest')
    if not Path(manifest['pm3Client']).is_file():
        raise ValueError('The explicitly configured PM3 client is missing')
    if not Path(manifest['pm3Port']).exists():
        raise ValueError('PM3 USB port is absent; physical reconnection is required before recovery')
    destination = Path(destination).resolve()
    if any(character in str(destination) for character in '\r\n;"\\'):
        raise ValueError('Recovery output path contains ambiguous PM3 CLI characters')
    destination.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='pm3-recovery-', dir=destination))
    (root / 'requested-original.bin').write_bytes(backup)
    report = {'ready': False, 'ramRestored': False, 'manifest': str(manifest_path),
              'requestedRamSha256': sha256(backup), 'outputDirectory': str(root)}
    before = None
    metadata = None
    url = manifest['ultraURL'].rstrip('/')

    def save_report():
        temporary = root / 'result.tmp'
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(root / 'result.json')

    def command(label, text):
        report['phase'] = label
        save_report()
        print('PM3 recovery: ' + label, flush=True)
        try:
            result = execute([manifest['pm3Client'], manifest['pm3Port'], '-c', text],
                             capture_output=True, timeout=12, check=True)
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
            (root / (label + '.log')).write_bytes((error.stdout or b'') + (error.stderr or b''))
            raise
        (root / (label + '.log')).write_bytes(result.stdout + result.stderr)
        return result.stdout

    def read_ram(label):
        target = root / label
        command(label, 'hf mf esave --4k -f ' + json.dumps(str(target)))
        data = target.with_suffix('.bin').read_bytes()
        if len(data) != 4096:
            raise RuntimeError('PM3 RAM readback does not contain exactly 4096 bytes')
        return data

    try:
        report['phase'] = 'identify Ultra'
        before = http(url + '/api/device')
        if before.get('chip') != manifest['ultraChip'] or before.get('busy') or before.get('reader'):
            raise RuntimeError('Expected idle Ultra identity/mode not confirmed; no PM3 commands sent')
        metadata = [http(f'{url}/api/slot/{slot}') for slot in range(8)]
        command('stop', 'hw break')
        ping = command('ping', 'hw ping')
        plain = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', ping)
        if not re.search(rb'Ping response received.*content \(\s*ok\s*\)', plain):
            raise RuntimeError('PM3 client exited without a confirmed ping response')
        report['pingConfirmed'] = True
        current = read_ram('current-before-restore')
        report['currentRamSha256'] = sha256(current)
        report['ramLoadIssued'] = current != backup
        if current != backup:
            command('restore-original', 'hf mf eload --4k -f ' + json.dumps(str(root / 'requested-original.bin')))
        restored = read_ram('verified-original')
        if restored != backup:
            raise RuntimeError('PM3 restored RAM differs from the requested backup')
        report['ramRestored'] = True
        report['verifiedRamSha256'] = sha256(restored)
        report['phase'] = 'verify RF absence'
        save_report()
        session = http(url + '/api/session', b'')['session']
        try:
            absent = http(f'{url}/api/command/2000?id={session}', b'')
            report['postStopScan'] = absent.hex()
            if absent != b'\x00\x01':
                raise RuntimeError('RF absence not confirmed; remove tags or recover the simulation before continuing')
        finally:
            http(f'{url}/api/end?id={session}', b'')
        report['rfAbsenceConfirmed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        try:
            if before is not None and metadata is not None:
                after = http(url + '/api/device')
                for key in ('chip', 'git', 'reader', 'active_slot', 'settings'):
                    if after.get(key) != before.get(key):
                        raise RuntimeError('Ultra state changed during recovery: ' + key)
                if after.get('busy'):
                    raise RuntimeError('Ultra reservation remains occupied after recovery')
                if [http(f'{url}/api/slot/{slot}') for slot in range(8)] != metadata:
                    raise RuntimeError('Ultra slot metadata changed during recovery')
                report['ultraPreserved'] = True
                report['firmware'] = after.get('git')
            report['ready'] = all(report.get(key) for key in (
                'pingConfirmed', 'ramRestored', 'rfAbsenceConfirmed', 'ultraPreserved')) and 'error' not in report
        except Exception as error:
            report['ready'] = False
            report['error'] = str(error)
            raise
        finally:
            report['terminal'] = True
            save_report()
    print(json.dumps({'ready': report['ready'], 'result': str(root / 'result.json')}), flush=True)
    return report


if __name__ == '__main__':
    recover(*sys.argv[1:])
