"""Read-only PM3 reported-firmware identity, separate from RF qualification."""
import hashlib
import json
from pathlib import Path
import re
import subprocess


SECTIONS = ('Model', 'ARM', 'FPGA', 'Hardware')
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def identity_digest(identity):
    if not isinstance(identity, dict) or set(identity) != set(SECTIONS):
        raise ValueError('Incomplete PM3 reported identity')
    for section in SECTIONS:
        lines = identity[section]
        if (not isinstance(lines, list) or not lines or
                any(not isinstance(line, str) or not line.strip() or len(line) > 1024 for line in lines)):
            raise ValueError('Invalid PM3 reported identity section')
    if (sum(line.startswith('Bootrom.... ') for line in identity['ARM']) != 1 or
            sum(line.startswith('OS......... ') for line in identity['ARM']) != 1 or
            not any(line.startswith('Firmware.................. ') for line in identity['Model']) or
            any(not re.fullmatch(r'fpga_\S+\.ncd image .+', line) for line in identity['FPGA']) or
            not any(line.startswith('--= uC: ') for line in identity['Hardware'])):
        raise ValueError('Missing PM3 firmware, FPGA or processor identity')
    return hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def parse_identity(output):
    if len(output) > 65536:
        raise ValueError('PM3 version output is too large')
    text = ANSI.sub('', output.decode('utf-8', errors='strict'))
    sections = {}
    current = None
    for line in text.splitlines():
        value = line.strip()
        heading = re.fullmatch(r'\[\s*(Model|ARM|FPGA|Hardware)\s*\]', value)
        if heading:
            current = heading.group(1)
            if current in sections:
                raise ValueError('Duplicate PM3 version section')
            sections[current] = []
        elif not value:
            current = None
        elif current is not None:
            # Diagnostic warnings and session paths are not firmware identity.
            if value.startswith('['):
                current = None
            else:
                sections[current].append(value)
    identity_digest(sections)
    return sections


def read_identity(executable, port, *, execute=None, logfile=None):
    try:
        result = (execute or subprocess.run)(
            [str(executable), port, '-c', 'hw version'],
            capture_output=True, timeout=12, check=True,
        )
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        if logfile is not None:
            Path(logfile).write_bytes((error.stdout or b'') + (error.stderr or b''))
        raise
    if logfile is not None:
        Path(logfile).write_bytes(result.stdout + result.stderr)
    return parse_identity(result.stdout)
