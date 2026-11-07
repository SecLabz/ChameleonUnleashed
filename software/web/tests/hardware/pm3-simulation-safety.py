"""Physical preflight: prove normal, killed-owner and deadline PM3 cleanup.

Also destroy the owner after changing emulator RAM, requiring the detached
supervisor to restore the frozen backup and independently checking all 4K.

Usage: python3 pm3-simulation-safety.py PM3_CLIENT PM3_PORT OUTPUT_DIRECTORY [classic|classic-mini|classic-2k|classic-4k]
Requires the HF-aligned Ultra at its USB HTTP address. No RF tag is written.
CHAMELEON_PM3_TEST_PERSISTENT_STOP=1 additionally requires the held-channel ping.
"""
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import urllib.request
from pm3_classic_qualification import CLASSIC_SCANS
from pm3_identity import identity_digest, read_identity


def request(path, body=None):
    req = urllib.request.Request('http://192.168.16.1' + path, data=body,
                                 headers={'Content-Type': 'application/octet-stream'})
    with urllib.request.urlopen(req, timeout=5) as response:
        data = response.read()
        return json.loads(data) if 'json' in response.headers.get('Content-Type', '') else data


def scan():
    session = request('/api/session', b'')['session']
    try:
        return request(f'/api/command/2000?id={session}', b'').hex()
    finally:
        request(f'/api/end?id={session}', b'')


def main():
    sizes = {'classic': '1k', 'classic-mini': 'mini', 'classic-2k': '2k', 'classic-4k': '4k'}
    if len(sys.argv) not in (4, 5) or (len(sys.argv) == 5 and sys.argv[4] not in sizes):
        raise ValueError('Optional simulation family must be classic, classic-mini, classic-2k or classic-4k')
    executable, port, destination = sys.argv[1:4]
    classic = len(sys.argv) == 5
    size = sizes[sys.argv[4]] if classic else None
    command = f'hf mf sim --{size} -u 11223344 -i' if classic else 'hf 14a sim -t 1 -u 11223344'
    expected_scan = CLASSIC_SCANS[size] if classic else CLASSIC_SCANS['1k']
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='pm3-stop-', dir=destination))
    helper = Path(__file__).with_name('pm3-simulation.py')
    executable = str(Path(executable).resolve())

    def pm3(command, label):
        result = subprocess.run([executable, port, '-c', command], capture_output=True,
                                timeout=15, check=True)
        (root / (label + '.log')).write_bytes(result.stdout + result.stderr)

    def save_ram(label):
        target = root / label
        pm3(f'hf mf esave --4k -f {json.dumps(str(target))}', label)
        data = target.with_suffix('.bin').read_bytes()
        assert len(data) == 4096, len(data)
        return data

    identity_before = read_identity(executable, port, logfile=root / 'pm3-identity-before.log')
    before = request('/api/device')
    assert not before['busy'] and not before['reader']
    results = []
    for index, mode in enumerate(('normal EOF', 'owner SIGKILL', 'hard deadline', 'RAM restore after owner SIGKILL')):
        logfile = str(root / (mode.replace(' ', '-') + '.log'))
        original = None
        ram_prefix = f'ram-{index}'
        restore_path = root / (ram_prefix + '-before.bin')
        if classic or mode == 'RAM restore after owner SIGKILL':
            original = save_ram(ram_prefix + '-before')
        if mode == 'RAM restore after owner SIGKILL':
            changed = bytes(byte ^ 0x5a for byte in original)
            (root / (ram_prefix + '-fixture.bin')).write_bytes(changed)
        env = {**os.environ, 'CHAMELEON_PM3_SIM_MAX_SECONDS': '5' if mode == 'hard deadline' else '30',
               'CHAMELEON_PM3_SIM_RESTORE_BIN': str(restore_path) if original is not None else '',
               'CHAMELEON_PM3_TEST_CLASSIC_STOP': '1' if classic else ''}
        if classic:
            env['CHAMELEON_PM3_TEST_PERSISTENT_STOP'] = '1'
            env['CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT'] = size
        owner = None
        record = None
        try:
            if mode == 'RAM restore after owner SIGKILL':
                pm3(f'hf mf eload --4k -f {json.dumps(str(root / (ram_prefix + "-fixture.bin")))}', 'ram-load')
                assert save_ram(ram_prefix + '-loaded') == changed
            owner = subprocess.Popen(
                [sys.executable, str(helper), executable, port,
                 command, logfile], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True, env=env,
            )
            assert select.select([owner.stdout], [], [], 8)[0], 'No simulation readiness event'
            event = owner.stdout.readline()
            assert event and json.loads(event)['ready'], event
            positive = scan()
            assert positive == expected_scan, positive
            started = time.monotonic()
            if mode == 'normal EOF':
                owner.stdin.close()
                owner.stdin = None
            elif 'owner SIGKILL' in mode:
                owner.kill()
            report = Path(logfile + '.cleanup.json')
            deadline = time.monotonic() + 70
            while not report.exists():
                assert time.monotonic() < deadline, 'Stop supervisor did not finish'
                time.sleep(0.1)
            cleanup = json.loads(report.read_text())
            assert cleanup['stopped'], cleanup
            if original is not None:
                assert cleanup['ramRestored'], cleanup
                assert cleanup['ramSha256'] == hashlib.sha256(original).hexdigest(), cleanup
                assert save_ram(ram_prefix + '-supervisor-restored') == original
            negative = scan()
            assert negative == '0001', negative
            if env.get('CHAMELEON_PM3_TEST_PERSISTENT_STOP') == '1':
                assert cleanup.get('persistentStopPingConfirmed'), 'Legacy fallback stopped RF, but the persistent channel did not qualify'
            record = {'mode': mode, 'cleanup': cleanup,
                            'elapsedSeconds': round(time.monotonic() - started, 3),
                            'positiveScan': positive, 'negativeScan': negative,
                            'maximumSeconds': int(env['CHAMELEON_PM3_SIM_MAX_SECONDS']),
                            'originalRamSha256': hashlib.sha256(original).hexdigest() if original is not None else None}
            results.append(record)
        finally:
            try:
                if owner is not None:
                    output, error = owner.communicate(timeout=75)
                    if record is not None:
                        record['ownerCode'] = owner.returncode
                    if mode == 'normal EOF':
                        assert owner.returncode == 0, (output, error)
                    elif 'owner SIGKILL' in mode:
                        assert owner.returncode == -9, (owner.returncode, output, error)
                    else:
                        assert owner.returncode != 0, (output, error)
            finally:
                if original is not None:
                    current = save_ram(ram_prefix + '-after')
                    if current != original:
                        pm3(f'hf mf eload --4k -f {json.dumps(str(restore_path))}', 'ram-fallback')
                        assert save_ram(ram_prefix + '-fallback-verified') == original
                        raise RuntimeError('RAM required fallback restoration; preflight failed')
        identity = read_identity(executable, port, logfile=root / f'pm3-identity-after-{index}.log')
        assert identity == identity_before, 'PM3 reported identity changed during stop qualification'
        record['pm3IdentitySha256'] = identity_digest(identity)
        print(json.dumps(record), flush=True)
    after = request('/api/device')
    for key in ('chip', 'git', 'settings', 'reader', 'active_slot'):
        assert before[key] == after[key], key
    assert not after['busy']
    identity_after = read_identity(executable, port, logfile=root / 'pm3-identity-after.log')
    assert identity_before == identity_after, 'PM3 reported identity changed during stop qualification'
    (root / 'results.json').write_text(json.dumps({'before': before, 'after': after, 'command': command,
                                                 'pm3Identity': identity_before, 'pm3IdentityAfter': identity_after,
                                                 'port': port, 'clientSha256': hashlib.sha256(Path(executable).read_bytes()).hexdigest(),
                                                 'results': results}, indent=2) + '\n')
    print(root, flush=True)


if __name__ == '__main__':
    main()
