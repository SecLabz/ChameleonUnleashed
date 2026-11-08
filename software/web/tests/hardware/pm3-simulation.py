"""Bounded PM3 simulations with an independent, detached stop supervisor.

The supervisor is armed before the PM3 client starts. EOF from its owner or a
hard deadline kills the serial-owning client, sends hw break and checks a real
ping response. Killing a CLI process alone does not stop PM3 firmware simulation.
Optional emulator RAM restoration uses a 4K backup frozen before arming.
"""
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import subprocess
import sys
import tempfile
import time
from pm3_stop_channel import StopChannel
from pm3_classic_qualification import classic_size, require_stop_report


MAX_SECONDS = int(os.environ.get('CHAMELEON_PM3_SIM_MAX_SECONDS', '180'))
if not 5 <= MAX_SECONDS <= 180:
    raise ValueError('PM3 simulation lifetime must be between 5 and 180 seconds')


def cleanup(executable, port, logfile, pid, reason, restore=None, stop_channel=None):
    result = {'reason': reason, 'serialOwnerPid': pid, 'stopped': False}
    if pid:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    output = bytearray()
    try:
        if stop_channel is not None:
            try:
                stop_channel.stop(timeout=15 if os.environ.get('CHAMELEON_PM3_TEST_CLASSIC_STOP') == '1' else 5)
                result['persistentStopPingConfirmed'] = True
            except Exception as error:
                # The accepted hf 14a family retains its already-qualified CLI
                # stop path. A fallback cannot qualify the experimental channel.
                result['persistentStopPingConfirmed'] = False
                result['persistentStopError'] = str(error)
            finally:
                output.extend(stop_channel.received)
        for command in ('hw break', 'hw ping'):
            reply = subprocess.run(
                [executable, port, '-c', command], capture_output=True,
                timeout=10, check=True,
            )
            output.extend(reply.stdout + reply.stderr)
            if command == 'hw ping':
                plain = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', reply.stdout)
                if not re.search(rb'Ping response received.*content \(\s*ok\s*\)', plain):
                    raise RuntimeError('PM3 ping was not confirmed')
        if restore is not None:
            Path(logfile).parent.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix='ram-restore-', dir=Path(logfile).parent))
            source = directory / 'original.bin'
            source.write_bytes(restore)
            target = directory / 'verified'
            for command in (f'hf mf eload --4k -f {json.dumps(str(source))}',
                            f'hf mf esave --4k -f {json.dumps(str(target))}'):
                reply = subprocess.run([executable, port, '-c', command],
                                       capture_output=True, timeout=10, check=True)
                output.extend(reply.stdout + reply.stderr)
            verified = target.with_suffix('.bin').read_bytes()
            if verified != restore:
                raise RuntimeError('PM3 emulator RAM restoration did not match the backup')
            result['ramRestored'] = True
            result['ramSha256'] = hashlib.sha256(verified).hexdigest()
        result['stopped'] = True
    except Exception as error:
        if isinstance(error, (subprocess.TimeoutExpired, subprocess.CalledProcessError)):
            output.extend(error.stdout or b'')
            output.extend(error.stderr or b'')
        result['error'] = str(error)
        raise
    finally:
        Path(logfile + '.cleanup.log').write_bytes(output)
        report = Path(logfile + '.cleanup.json')
        temporary = report.with_suffix('.tmp')
        temporary.write_text(json.dumps(result, indent=2) + '\n')
        temporary.replace(report)
    return result


def supervise(executable, port, logfile):
    pid = None
    started = time.monotonic()
    pending = bytearray()
    restore_path = os.environ.get('CHAMELEON_PM3_SIM_RESTORE_BIN')
    restore = Path(restore_path).read_bytes() if restore_path else None
    if restore is not None and len(restore) != 4096:
        raise ValueError('PM3 emulator RAM backup must contain exactly 4096 bytes')
    # Opt-in held descriptor: qualified for hf 14a and the exact guarded Classic 1K mode.
    stop_channel = StopChannel(port) if os.environ.get('CHAMELEON_PM3_TEST_PERSISTENT_STOP') == '1' else None
    print('armed', flush=True)
    reason = 'owner EOF'
    while True:
        remaining = MAX_SECONDS - (time.monotonic() - started)
        if remaining <= 0:
            reason = 'hard deadline'
            break
        readable, _, _ = select.select([sys.stdin], [], [], min(remaining, 0.25))
        if not readable:
            continue
        data = os.read(sys.stdin.fileno(), 4096)
        if not data:
            break
        pending.extend(data)
        while b'\n' in pending:
            line, _, pending = pending.partition(b'\n')
            message = json.loads(line)
            pid = message['pid']
            print('registered', flush=True)
    try:
        result = cleanup(executable, port, logfile, pid, reason, restore, stop_channel)
    finally:
        if stop_channel is not None:
            stop_channel.close()
    print(json.dumps(result), flush=True)


def validate_command(command, executable=None, port=None):
    size = classic_size(command)
    classic = size is not None
    if (not command.startswith('hf 14a sim ') and not classic) or any(char in command for char in ';\r\n'):
        raise ValueError('Only hf 14a sim or an exact guarded Classic command is accepted')
    if classic and (os.environ.get('CHAMELEON_PM3_TEST_CLASSIC_STOP') != '1' or
                    os.environ.get('CHAMELEON_PM3_TEST_PERSISTENT_STOP') != '1' or
                    not os.environ.get('CHAMELEON_PM3_SIM_RESTORE_BIN')):
        raise ValueError('Classic requires explicit opt-in, a held stop descriptor and a full RAM backup')
    if classic and size != '1k':
        preflight = os.environ.get('CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT')
        if preflight == size and MAX_SECONDS <= 30:
            # This bounded experiment produces the evidence. Routine reader
            # campaigns must instead supply the completed four-case report.
            return True
        require_stop_report(command, os.environ.get('CHAMELEON_PM3_CLASSIC_STOP_REPORT'), executable, port, live=True)
    return classic


def main(executable, port, command, logfile):
    validate_command(command, executable, port)
    guard = subprocess.Popen(
        [sys.executable, __file__, '--supervise', executable, port, logfile],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    child = None
    master = None
    launch_read = None
    launch_write = None
    output = bytearray()
    try:
        if not select.select([guard.stdout], [], [], 5)[0] or guard.stdout.readline() != b'armed\n':
            raise RuntimeError('PM3 stop supervisor did not arm')
        master, slave = pty.openpty()
        launch_read, launch_write = os.pipe()
        child = subprocess.Popen(
            [sys.executable, __file__, '--launch', str(launch_read),
             executable, port, '-f', '-c', command], stdin=slave,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True, pass_fds=(launch_read,),
        )
        os.close(slave)
        os.close(launch_read)
        launch_read = None
        guard.stdin.write(json.dumps({'pid': child.pid}).encode() + b'\n')
        guard.stdin.flush()
        if not select.select([guard.stdout], [], [], 5)[0] or guard.stdout.readline() != b'registered\n':
            raise RuntimeError('PM3 client PID was not registered with its supervisor')
        # The wrapper cannot open USB until its PID is known to the supervisor.
        # If this owner dies before registration, pipe EOF exits the wrapper.
        os.write(launch_write, b'G')
        os.close(launch_write)
        launch_write = None
        ready = False
        started = time.monotonic()
        while True:
            # Do not poll/wait/reap this child until the guard finishes. Its
            # reserved PID cannot be reused for an unrelated process.
            if time.monotonic() - started > (MAX_SECONDS if ready else 15):
                raise TimeoutError('PM3 simulation owner deadline expired')
            readable, _, _ = select.select([child.stdout, sys.stdin], [], [], 0.25)
            if sys.stdin in readable:
                break
            if child.stdout in readable:
                data = os.read(child.stdout.fileno(), 4096)
                if not data:
                    raise RuntimeError('PM3 output closed before stop')
                output.extend(data)
                if not ready and b'abort simulation' in output:
                    ready = True
                    print(json.dumps({'ready': True, 'supervisorPid': guard.pid,
                                      'serialOwnerPid': child.pid}), flush=True)
    finally:
        for descriptor in (launch_read, launch_write):
            if descriptor is not None:
                os.close(descriptor)
        # The separate session also reaches this cleanup if its owner is
        # SIGKILLed: the guard observes EOF on the only open writer of its pipe.
        guard_output, guard_error = guard.communicate(timeout=75)
        if child:
            output.extend(child.communicate(timeout=5)[0])
        if master is not None:
            os.close(master)
        Path(logfile).write_bytes(output)
        if guard.returncode != 0 or not json.loads(guard_output)['stopped']:
            raise RuntimeError(f'Independent PM3 cleanup failed: {guard_error!r}')


if __name__ == '__main__':
    if sys.argv[1] == '--supervise':
        supervise(*sys.argv[2:])
    elif sys.argv[1] == '--launch':
        descriptor = int(sys.argv[2])
        permitted = os.read(descriptor, 1) == b'G'
        os.close(descriptor)
        if permitted:
            os.execv(sys.argv[3], sys.argv[3:])
    else:
        main(*sys.argv[1:])
