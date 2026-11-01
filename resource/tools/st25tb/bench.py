"""Qualify repeated PM3 reads; save raw evidence and fail on any retry or mismatch."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import rig


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sessions', type=int, default=30)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--command', default='hf 14b dump --ns')
    parser.add_argument('--fixture', type=Path, default=rig.FIXTURE)
    parser.add_argument('--physical-tag', action='store_true',
                        help='validate selection against each CRC-valid INITIATE reply')
    parser.add_argument('--output', type=Path, help='new evidence directory (default .cache/st25tb-pm3/TIMESTAMP)')
    parser.add_argument('--label', default='', help='flashed build/package identifier recorded with evidence')
    args = parser.parse_args(argv)
    if args.sessions < 1 or args.repeat < 1:
        parser.error('--sessions and --repeat must be positive')
    try:
        rig.command_kind(args.command)
        memory = rig.image(args.fixture)
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        destination = args.output or Path('.cache/st25tb-pm3') / stamp
        destination.mkdir(parents=True, exist_ok=False)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'{error}\n')

    report = {'started_utc': stamp, 'label': args.label, 'command': args.command,
              'sessions_requested': args.sessions, 'repeat': args.repeat,
              'fixture': str(args.fixture.resolve()),
              'fixture_sha256': hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
              'physical_tag': args.physical_tag,
              'pm3_client': os.environ.get('PM3_CLIENT', 'proxmark3'),
              'source': subprocess.run(['git', 'describe', '--always', '--dirty'], capture_output=True,
                                       text=True).stdout.strip(),
              'sessions': [], 'errors': []}
    started = time.monotonic()
    print(f'Evidence: {destination}', flush=True)
    try:
        report['pm3_port'] = rig.pm3_port()
        (destination / 'pm3-version.txt').write_text(rig.pm3('hw version'))
        for session in range(1, args.sessions + 1):
            # Clearing once per session prevents trace exhaustion while retaining
            # every exchange between the back-to-back commands under test.
            commands = ['hf 14b reader'] + [args.command] * args.repeat + ['trace list -t 14b']
            raw = rig.pm3('; '.join(commands))
            (destination / f'session-{session:04}.txt').write_text(raw)
            outputs = rig.split_commands(raw, len(commands))
            results = [rig.check_output(output, args.command, memory) for output in outputs[1:-1]]
            trace, _ = rig.trace_result(outputs[-1], memory, physical_tag=args.physical_tag)
            rig.check_trace_coverage(trace, args.command, args.repeat)
            entry = {'number': session, 'commands': results, 'trace': trace}
            report['sessions'].append(entry)
            states = []
            for result in results:
                state = 'FAIL' if result['errors'] else 'ok'
                if result['retries']:
                    state += f"/r{result['retries']}"
                states.append(state)
            print(f"session {session:3}: {' '.join(states)}; "
                  f"trace {trace['ok']}/{trace['total']}", flush=True)
            (destination / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as error:
        report['errors'].append(str(error) or 'Interrupted')
        (destination / 'error.txt').write_text(str(error))
    finally:
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        report['passed'] = (not report['errors'] and len(report['sessions']) == args.sessions
                            and all(not item['trace']['errors']
                                    and all(not command['errors'] and not command['retries']
                                            for command in item['commands'])
                                    for item in report['sessions']))
        (destination / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"{'PASS' if report['passed'] else 'FAIL'}; {report['elapsed_seconds']:.1f}s; "
          f"{destination / 'result.json'}")
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
