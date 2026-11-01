"""PM3 transport and strict ST25TB512-AT qualification checks; no hardware on import."""

import collections
import glob
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

FIXTURE = Path(__file__).resolve().parents[1] / 'tests/st25tb/slot.json'
COMMANDS = {0x06: 'INITIATE', 0x0E: 'SELECT', 0x0B: 'GET_UID', 0x08: 'READ_BLK'}
BLOCKS = (*range(16), 255)
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
HEX = r'(?:[0-9a-fA-F]{2}\s*)'


def clean(text):
    return ANSI.sub('', text)


def image(path=FIXTURE):
    document = json.loads(Path(path).read_text())
    memory = bytes(document['memory'])
    if document['tag_type'] != 2000 or len(memory) != 77:
        raise ValueError('Expected a 77-byte ST25TB512-AT slot image')
    return memory


def pm3_port():
    if os.environ.get('PM3_PORT'):
        return os.environ['PM3_PORT']
    ports = sorted(glob.glob('/dev/cu.usbmodemiceman*'))
    if len(ports) != 1:
        raise ValueError('Set PM3_PORT to the Proxmark3 port (required on Linux or with multiple PM3s)')
    return ports[0]


def pm3(command, timeout=120):
    """Preserve output on process errors and timeouts for qualification evidence."""
    client = os.environ.get('PM3_CLIENT', 'proxmark3')
    try:
        result = subprocess.run([client, pm3_port(), '-c', command], capture_output=True,
                                text=True, errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b''
        if isinstance(output, bytes):
            output = output.decode(errors='replace')
        raise RuntimeError(f'PM3 timed out after {timeout}s\n{output}') from error
    output = result.stdout + result.stderr
    if result.returncode:
        raise RuntimeError(f'PM3 exited {result.returncode}\n{output}')
    return output


def split_commands(output, count):
    parts = re.split(r'(?:\[[^\]\r\n]+\]\s*)?pm3 -->[^\r\n]*(?:\r?\n|$)', clean(output))[1:]
    if len(parts) != count:
        raise ValueError(f'Expected {count} PM3 command outputs, got {len(parts)}')
    return parts


def command_kind(command):
    words = shlex.split(command)
    if words[:3] == ['hf', '14b', 'dump'] and words[3:] == ['--ns']:
        return 'dump', None
    if words == ['hf', '14b', 'info']:
        return 'info', None
    if len(words) == 5 and words[:4] == ['hf', '14b', 'rdbl', '-b']:
        block = int(words[4])
        if 0 <= block <= 15 or block == 255:
            return 'rdbl', block
    raise ValueError('Use hf 14b dump --ns, hf 14b info or hf 14b rdbl -b BLOCK')


def block_value(memory, block):
    offset = 8 + 4 * (16 if block == 255 else block)
    return memory[offset:offset + 4]


def check_output(output, command, memory):
    """Check observable UID, every dump block, or the requested block value."""
    output = clean(output)
    kind, block = command_kind(command)
    errors = []
    if re.search(r'no tag found|dump failed|failed to select|timeout|timed out', output, re.I):
        errors.append('PM3 reported a failed exchange')
    # This PM3 client omits the UID in a dump; GET_UID trace bytes prove it.
    if kind == 'info':
        uids = re.findall(r'UID:\s*((?:[0-9a-fA-F]{2}\s*){8})(?![0-9a-fA-F])', output)
        if not uids or any(bytes.fromhex(uid) != memory[:8][::-1] for uid in uids):
            errors.append('UID missing or differs from fixture')
        if not re.search(r'Chip:\s*0C\s*,\s*SRT512\b', output, re.I):
            errors.append('SRT512 identity missing')
    elif kind == 'dump':
        rows = re.findall(r'(\d+)/0x([0-9a-fA-F]{2})\s*\|\s*(' + HEX + r'{4})\|', output)
        blocks = {}
        for decimal, hexadecimal, value in rows:
            address = int(decimal)
            if address in blocks or address != int(hexadecimal, 16):
                errors.append('Duplicate or inconsistent block address')
            blocks[address] = bytes.fromhex(value)
        if set(blocks) != set(BLOCKS):
            errors.append('Incomplete dump (requires blocks 0..15 and 255)')
        for address in BLOCKS:
            if address in blocks and blocks[address] != block_value(memory, address):
                errors.append(f'Block {address} differs from fixture')
    else:
        rows = re.findall(r'block\s+(\d+)\.\.\.\s*(' + HEX + r'{4})\|', output, re.I)
        if len(rows) != 1 or int(rows[0][0]) != block or bytes.fromhex(rows[0][1]) != block_value(memory, block):
            errors.append(f'Block {block} missing or differs from fixture')
    return {'errors': errors, 'retries': len(re.findall(r'retrying', output, re.I))}


def valid_crc(frame):
    if len(frame) < 3:
        return False
    crc = 0xFFFF
    for value in frame[:-2]:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return frame[-2:] == ((~crc) & 0xFFFF).to_bytes(2, 'little')


def score_trace(text, memory, *, physical_tag=False):
    """Score complete PM3 rows, CRC-B, reply lengths and fixture bytes.

    Every table row is retained, including malformed rows, so an invalid request
    cannot disappear and let its response satisfy an earlier missing exchange.
    Unknown non-ST25TB reader commands are ignored; they still break adjacency.
    Physical tags announce their selection byte in each valid INITIATE reply;
    emulator qualification keeps the fixture's exact fixed selection byte.
    """
    rows = []
    counts = collections.Counter()
    starts = []
    inventory = None
    for line in clean(text).splitlines():
        if not re.search(r'\|\s*(Rdr|Tag)\s*\|', line):
            continue
        parts = [part.strip() for part in line.split('|')]
        valid = (len(parts) >= 5 and parts[0].isdigit() and parts[1].isdigit()
                 and re.fullmatch(r'(?:\s*[0-9a-fA-F]{2}\s*|[\[\]])+', parts[3]) is not None)
        if not valid:
            counts['invalid'] += 1
            rows.append(None)
            continue
        data = bytes.fromhex(parts[3].replace('[', '').replace(']', ''))
        if not data:
            counts['invalid'] += 1
            rows.append(None)
            continue
        rows.append((parts[2], data, parts[4], int(parts[0])))

    for index, row in enumerate(rows):
        if row is None:
            continue
        if row[0] == 'Tag':
            # An invalid preceding row already fails the trace. Otherwise every
            # reply must follow a reader row; an extra tag frame is not invisible.
            previous = rows[index - 1] if index else None
            if index == 0 or (previous is not None and previous[0] != 'Rdr'):
                counts['invalid'] += 1
            continue
        if row[1][0] not in COMMANDS:
            continue
        command = row[1][0]
        request = row[1]
        if physical_tag and command == 0x06:
            inventory = None
        expected_length = 3 if command == 0x0B else 4
        request_ok = len(request) == expected_length and row[2] == 'ok' and valid_crc(request)
        if physical_tag and command == 0x0E:
            request_ok = request_ok and inventory is not None and request[1:2] == inventory
        if not request_ok:
            counts['request_bad'] += 1
        elif command == 0x08:
            counts[('READ_BLK', request[1])] += 1
        reply = rows[index + 1] if index + 1 < len(rows) else None
        if reply is None or reply[0] != 'Tag':
            kind = 'missing'
        else:
            expected = inventory if physical_tag else memory[76:77]
            if physical_tag and command == 0x06:
                # A physical tag announces a new selection byte in INITIATE.
                # Learn it only after the request, reply length and CRC pass.
                expected = reply[1][:1]
            if command == 0x0B:
                expected = memory[:8]
            elif command == 0x08:
                expected = b''
                if request_ok and (request[1] < 16 or request[1] == 255):
                    expected = block_value(memory, request[1])
            reply_ok = (request_ok and reply[2] == 'ok' and valid_crc(reply[1])
                        and reply[1][:-2] == expected)
            if physical_tag and command == 0x06 and reply_ok:
                inventory = expected
            kind = 'ok' if reply_ok else 'bad'
            starts.append(reply[3])
        counts[kind] += 1
        counts[(COMMANDS[command], kind)] += 1
    return counts, starts


def trace_result(text, memory, *, physical_tag=False):
    counts, starts = score_trace(text, memory, physical_tag=physical_tag)
    result = {key: counts[key] for key in ('ok', 'bad', 'missing', 'invalid', 'request_bad')}
    result['total'] = sum(counts[key] for key in ('ok', 'bad', 'missing'))
    result['commands'] = {name: {key: counts[(name, key)] for key in ('ok', 'bad', 'missing')}
                          for name in COMMANDS.values()}
    result['read_blocks'] = {str(block): counts[('READ_BLK', block)] for block in BLOCKS}
    result['errors'] = []
    if not result['total']:
        result['errors'].append('No ST25TB exchanges found')
    if any(result[key] for key in ('bad', 'missing', 'invalid', 'request_bad')):
        result['errors'].append('Trace contains failed or malformed exchanges')
    return result, starts


def check_trace_coverage(result, command, repeat):
    """Fail closed on a truncated trace even if the retained exchanges are valid."""
    kind, block = command_kind(command)
    selections = 1 + repeat * (2 if kind == 'dump' else 1)
    expected = {'INITIATE': selections, 'SELECT': selections, 'GET_UID': selections}
    for name, minimum in expected.items():
        observed = sum(result['commands'][name].values())
        if observed < minimum:
            result['errors'].append(f'Trace incomplete: {name} {observed} < {minimum}')
    if kind == 'info':
        return
    blocks = BLOCKS if kind == 'dump' else (block,)
    for address in blocks:
        observed = result['read_blocks'][str(address)]
        if observed < repeat:
            result['errors'].append(f'Trace incomplete: READ_BLK({address}) {observed} < {repeat}')


def print_trace(result):
    print(f"exchanges {result['total']}: ok {result['ok']}, bad {result['bad']}, "
          f"missing {result['missing']}, invalid {result['invalid']}, bad request {result['request_bad']}")
    for name, counts in result['commands'].items():
        if any(counts.values()):
            print(f'  {name:9} {counts}')
