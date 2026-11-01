"""Read-only USB CDC stress during PM3 qualification; no hardware on import."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import queue
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'software/script'))
from chameleon_enum import Command, Status
from serial.tools import list_ports

INTERVAL_SECONDS = 0.1
TIMEOUT_SECONDS = 3
LATENCY_LIMIT_SECONDS = 2.5
COMMANDS = (Command.GET_APP_VERSION, Command.GET_DEVICE_MODE, Command.GET_DEVICE_CAPABILITIES)
REQUIRED_CAPABILITIES = {*COMMANDS, Command.ST25TB_GET_SLOT_IMAGE}


def create_connection():
    # Load the existing USB client only when starting hardware I/O.
    from chameleon_com import ChameleonCom

    return ChameleonCom()


def find_port(explicit):
    port = explicit or os.environ.get('CHAMELEON_PORT')
    if port:
        return port
    matches = [item.device for item in list_ports.comports() if (item.vid, item.pid) == (0x6868, 0x8686)]
    if len(matches) != 1:
        raise ValueError('Set --port or CHAMELEON_PORT to exactly one Chameleon Ultra CDC port')
    return matches[0]


def read_response(com, command):
    # The client starts its own timeout only after queue registration. This
    # deadline also covers a disconnect that drops the task before registration.
    deadline = time.monotonic() + TIMEOUT_SECONDS
    replies = queue.Queue(maxsize=1)

    def received(cmd, status, data):
        try:
            replies.put_nowait((time.monotonic(), cmd, status, data))
        except queue.Full:
            pass

    com.send_cmd_auto(int(command), callback=received, timeout=TIMEOUT_SECONDS)
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError(f'CMD {int(command)} response deadline exceeded')
    try:
        arrived, cmd, status, data = replies.get(timeout=remaining)
    except queue.Empty as error:
        raise TimeoutError(f'CMD {int(command)} response deadline exceeded') from error
    if arrived > deadline:
        raise TimeoutError(f'CMD {int(command)} response deadline exceeded')
    if status is None:
        raise TimeoutError(f'CMD {int(command)} response timeout')
    if cmd != command:
        raise ValueError(f'Unexpected response command {cmd}; expected {int(command)}')
    return status, data


def query(com, command, cycle, origin, latency_limit=LATENCY_LIMIT_SECONDS):
    started = time.monotonic()
    record = {'type': 'command', 'cycle': cycle, 'command': int(command), 'name': command.name,
              'started_monotonic_s': started, 'elapsed_from_start_s': started - origin,
              'errors': [], 'timeout': False, 'interrupted': False}
    try:
        status, data = read_response(com, command)
        record['status'] = int(status)
        record['data_hex'] = data.hex()
        if status != Status.SUCCESS:
            record['errors'].append(f'Status {status:#x}; expected {int(Status.SUCCESS):#x}')
        if command == Command.GET_APP_VERSION and len(data) != 2:
            record['errors'].append(f'Version length {len(data)}; expected 2')
        if command == Command.GET_DEVICE_MODE and data != b'\0':
            record['errors'].append(f'Mode {data.hex()}; expected 00 (emulator)')
        if command == Command.GET_DEVICE_CAPABILITIES:
            # Exercise a response spanning USB packets without fixing the whole
            # command map; new firmware commands may extend or reorder it.
            if len(data) <= 64 or len(data) % 2:
                record['errors'].append(f'Capabilities length {len(data)}; expected even and greater than 64')
            else:
                codes = [code for code, in struct.iter_unpack('!H', data)]
                missing = REQUIRED_CAPABILITIES - set(codes)
                if missing:
                    record['errors'].append(f'Capabilities missing required commands: {sorted(missing)}')
                if len(codes) != len(set(codes)):
                    record['errors'].append('Capabilities contain duplicate command codes')
    except KeyboardInterrupt:
        record['interrupted'] = True
    except Exception as error:
        record['exception'] = type(error).__name__
        record['timeout'] = isinstance(error, TimeoutError)
        record['errors'].append(str(error) or type(error).__name__)
    finally:
        record['duration_seconds'] = time.monotonic() - started
        if record['duration_seconds'] > latency_limit:
            record['errors'].append(f'Latency exceeded {latency_limit:.3f}s')
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duration', type=float, default=180, help='maximum run duration in seconds (default 180)')
    parser.add_argument('--max-latency', type=float, default=LATENCY_LIMIT_SECONDS,
                        help='command latency limit in seconds (default 2.5, must be below timeout 3)')
    parser.add_argument('--stop-file', type=Path, help='stop when this file exists; never deletes the file')
    parser.add_argument('--port', help='Chameleon CDC port (or CHAMELEON_PORT; otherwise USB auto-detection)')
    parser.add_argument('--output', type=Path, help='new output directory')
    parser.add_argument('--label', default='', help='flashed package SHA-256/identity')
    args = parser.parse_args(argv)
    if args.duration <= 0:
        parser.error('--duration must be positive')
    if not 0 < args.max_latency < TIMEOUT_SECONDS:
        parser.error('--max-latency must be positive and below the 3-second command timeout')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    output = args.output or ROOT / '.cache/st25tb-pm3/usb-stress' / stamp
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    summary = {'started_utc': stamp, 'label': args.label, 'duration_requested_seconds': args.duration,
               'interval_seconds': INTERVAL_SECONDS, 'timeout_seconds': TIMEOUT_SECONDS,
               'latency_limit_seconds': args.max_latency, 'queries': 0,
               'validated_queries': 0, 'interrupted_queries': 0, 'errors': 0, 'timeouts': 0,
               'max_duration_seconds': 0, 'commands': {str(int(command)): {'queries': 0,
                   'errors': 0, 'max_duration_seconds': 0} for command in COMMANDS},
               'stop_reason': None, 'connection_closed': False}
    com = create_connection()
    print(f'USB stress evidence: {output}', flush=True)
    with (output / 'commands.jsonl').open('w', buffering=1) as stream:
        def log(record):
            stream.write(json.dumps(record) + '\n')

        log({'type': 'metadata', **summary, 'stop_file': str(args.stop_file) if args.stop_file else None,
             'commands': [{'command': int(command), 'name': command.name} for command in COMMANDS]})
        try:
            summary['port'] = find_port(args.port)
            opening = time.monotonic()
            com.open(summary['port'])
            log({'type': 'connected', 'port': summary['port'],
                 'open_duration_seconds': time.monotonic() - opening})
            next_cycle = time.monotonic()
            cycle = 0
            while summary['stop_reason'] is None:
                if args.stop_file and args.stop_file.exists():
                    summary['stop_reason'] = 'stop_file'
                    break
                if time.monotonic() - started >= args.duration:
                    summary['stop_reason'] = 'duration'
                    break
                cycle += 1
                for command in COMMANDS:
                    if time.monotonic() - started >= args.duration:
                        summary['stop_reason'] = 'duration'
                        break
                    if args.stop_file and args.stop_file.exists():
                        summary['stop_reason'] = 'stop_file'
                        break
                    record = query(com, command, cycle, started, args.max_latency)
                    log(record)
                    summary['queries'] += 1
                    summary['interrupted_queries'] += record['interrupted']
                    summary['errors'] += bool(record['errors'])
                    summary['timeouts'] += record['timeout']
                    summary['max_duration_seconds'] = max(summary['max_duration_seconds'], record['duration_seconds'])
                    per_command = summary['commands'][str(int(command))]
                    per_command['queries'] += 1
                    per_command['errors'] += bool(record['errors'])
                    per_command['max_duration_seconds'] = max(per_command['max_duration_seconds'], record['duration_seconds'])
                    if record['errors']:
                        summary['stop_reason'] = 'error'
                        break
                    if record['interrupted']:
                        summary['stop_reason'] = 'sigint'
                        break
                    summary['validated_queries'] += 1
                if summary['stop_reason'] is not None:
                    break
                # A blocked command must not cause a burst of queued requests.
                next_cycle = max(next_cycle + INTERVAL_SECONDS, time.monotonic())
                time.sleep(max(0, min(next_cycle - time.monotonic(), args.duration - (time.monotonic() - started))))
        except KeyboardInterrupt:
            summary['stop_reason'] = 'sigint'
        except Exception as error:
            summary['errors'] += 1
            summary['stop_reason'] = 'error'
            log({'type': 'error', 'phase': 'connect_or_loop', 'exception': type(error).__name__,
                 'error': str(error), 'elapsed_from_start_s': time.monotonic() - started})
        finally:
            try:
                com.close()
                summary['connection_closed'] = True
            except Exception as error:
                summary['errors'] += 1
                log({'type': 'error', 'phase': 'cleanup', 'error': str(error)})
            summary['elapsed_seconds'] = time.monotonic() - started
            summary['passed_observed_queries'] = summary['errors'] == 0 and summary['validated_queries'] > 0
            log({'type': 'summary', **summary})
            (output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f"USB stress stopped ({summary['stop_reason']}): queries={summary['queries']}, "
          f"errors={summary['errors']}, timeouts={summary['timeouts']}, "
          f"max={summary['max_duration_seconds']:.3f}s; {output / 'result.json'}", flush=True)
    return int(summary['errors'] > 0)


if __name__ == '__main__':
    raise SystemExit(main())
