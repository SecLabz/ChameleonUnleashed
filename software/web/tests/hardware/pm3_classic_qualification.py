"""Exact Classic simulation commands and evidence required for new sizes."""
import hashlib
import json
from pathlib import Path
import re
import sys
from pm3_identity import identity_digest, read_identity


CLASSIC_SCANS = {
    '1k': '0000041122334404000800',
    'mini': '0000041122334404000900',
    '2k': '0000041122334404000800',
    '4k': '0000041122334402001800',
}
FAULTS = ('normal EOF', 'owner SIGKILL', 'hard deadline', 'RAM restore after owner SIGKILL')


def classic_size(command):
    match = re.fullmatch(r'hf mf sim --(1k|mini|2k|4k) -u [0-9a-fA-F]{8} -i', command)
    return match.group(1) if match else None


def require_stop_report(command, report_path, executable, port, *, live=False, execute=None):
    """Validate offline evidence first; optionally compare a read-only live identity."""
    if not report_path or not executable or not port:
        raise ValueError('New Classic sizes require a completed stop report, client and port')
    raw = Path(report_path).read_bytes()
    if len(raw) > 65536:
        raise ValueError('Classic stop report is too large')
    report = json.loads(raw)
    if not isinstance(report, dict):
        raise ValueError('Classic stop report must be an object')
    size = classic_size(command)
    if (report.get('command') != command or size not in CLASSIC_SCANS or
            report.get('port') != port or
            report.get('clientSha256') != hashlib.sha256(Path(executable).read_bytes()).hexdigest()):
        raise ValueError('Classic stop report does not match command, port or client')
    identity = report.get('pm3Identity')
    pm3_digest = identity_digest(identity)
    if report.get('pm3IdentityAfter') != identity:
        raise ValueError('PM3 reported identity changed during stop qualification')
    before, after = report.get('before', {}), report.get('after', {})
    if not isinstance(before, dict) or not isinstance(after, dict):
        raise ValueError('Classic stop report must contain device states')
    for key in ('chip', 'git', 'settings', 'reader', 'active_slot'):
        if key not in before or key not in after or before[key] != after[key]:
            raise ValueError('Classic stop report does not preserve Ultra state')
    if before.get('busy') is not False or after.get('busy') is not False or before['reader'] is not False:
        raise ValueError('Classic stop report leaves the Ultra busy or in reader mode')
    records = report.get('results', [])
    if not isinstance(records, list) or len(records) != len(FAULTS):
        raise ValueError('Classic stop report must contain all four fault cases')
    if any(not isinstance(r, dict) for r in records) or sorted(r.get('mode', '') for r in records) != sorted(FAULTS):
        raise ValueError('Classic stop report must contain four distinct fault cases')
    for record in records:
        cleanup = record.get('cleanup', {})
        if not isinstance(cleanup, dict):
            raise ValueError('Classic stop report must contain cleanup results')
        mode = record['mode']
        expected_lifetime = 5 if mode == 'hard deadline' else 30
        owner = record.get('ownerCode')
        owner_ok = type(owner) is int and (owner == 0 if mode == 'normal EOF' else
                    owner == -9 if 'owner SIGKILL' in mode else
                    owner != 0)
        digest = record.get('originalRamSha256', '')
        if (record.get('positiveScan') != CLASSIC_SCANS[size] or record.get('negativeScan') != '0001' or
                record.get('maximumSeconds') != expected_lifetime or not owner_ok or
                record.get('pm3IdentitySha256') != pm3_digest or
                not re.fullmatch(r'[0-9a-f]{64}', digest) or cleanup.get('ramSha256') != digest or
                any(cleanup.get(key) is not True for key in
                    ('stopped', 'persistentStopPingConfirmed', 'ramRestored'))):
            raise ValueError(f'Classic stop report failed {mode}')
    if live and read_identity(executable, port, execute=execute) != identity:
        raise ValueError('PM3 reported firmware identity changed; repeat the stop preflight')
    return report


if __name__ == '__main__':
    arguments = sys.argv[1:]
    live = arguments[:1] == ['--live']
    if live:
        arguments = arguments[1:]
    if len(arguments) != 4:
        raise ValueError('Usage: pm3_classic_qualification.py [--live] COMMAND REPORT CLIENT PORT')
    report = require_stop_report(*arguments, live=live)
    print(json.dumps({'qualified': True, 'command': arguments[0], 'liveIdentityChecked': live,
                      'pm3IdentitySha256': identity_digest(report['pm3Identity'])}))
