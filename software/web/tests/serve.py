#!/usr/bin/env python3
"""Serve embedded assets against a simulated FF-key card for browser checks."""

from copy import deepcopy
import argparse
import gzip
import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

SITE = Path(__file__).resolve().parents[3] / '.cache/web/assets/site'
CONFIG = json.loads((SITE.parent / 'config.json').read_text())
ADDRESS = urlsplit(CONFIG['baseUrl'])
RESOURCE_ORIGIN = ''
if ADDRESS.netloc:
    RESOURCE_ORIGIN = ADDRESS.scheme + '://' + ADDRESS.netloc

CSP = (
    "default-src 'self'; connect-src 'self'"
    + (' ' + RESOURCE_ORIGIN if RESOURCE_ORIGIN else '') + "; "
    "worker-src 'self'; script-src 'self' 'wasm-unsafe-eval'; "
    "style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'"
)


def block_bytes(block):
    if block == 0:
        return bytes([1, 2, 3, 4, 4, 8, 4, 0]) + bytes(8)

    blocks_per_sector = 4 if block < 128 else 16
    if block % blocks_per_sector == blocks_per_sector - 1:
        return b'\xff' * 6 + bytes([255, 7, 128, 105]) + b'\xff' * 6

    return bytes(16)


class SimulatedCard(BaseHTTPRequestHandler):
    offline = False
    storage_unavailable = False
    settings = {
        'animation': 0,
        'press_a': 1,
        'press_b': 2,
        'long_a': 3,
        'long_b': 4,
        'sleep': 8,
        'long_ms': 1000,
        'pairing': False,
    }
    initial_settings = deepcopy(settings)
    bank = [dict(hf=None, lf=None) for _ in range(8)]
    bank[0]['hf'] = dict(
        name='Test Classic',
        enabled=True,
        image=bytes([1, 3, 233, 16, 4, 0, 4, 0, 7, 1, 2, 3, 4, 4, 0, 8, 0, 0, 0, 0, 0, 0, 1])
        + b''.join(block_bytes(i) for i in range(64)),
    )
    initial_bank = deepcopy(bank)
    tag_kind = 'classic'
    classic_memory = [block_bytes(block) for block in range(256)]
    st25_uid = bytes([0xD0, 2, 0x33, 1, 2, 3, 4, 5])
    active = 0
    session = 0
    next_session = 0
    reader_session = False
    reader = False
    previous_reader = False
    staging = None
    snapshot = None
    lf_memory = [0x00148041, 0xFF800000, 0x0034A800, 0, 0, 0, 0]

    @classmethod
    def revision(cls, slot, sense):
        place = cls.bank[slot][sense]
        data = bytes([slot == cls.active])
        if place:
            data += bytes([place['enabled']]) + place['name'].encode() + place['image']

        return hashlib.sha256(data).digest()[:4]

    def respond(self, data, content='application/octet-stream', status=200, packed=False):
        policy = CSP
        if self.offline and RESOURCE_ORIGIN:
            policy = CSP.replace(' ' + RESOURCE_ORIGIN, '')

        self.send_response(status)
        self.send_header('Content-Type', content)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Content-Security-Policy', policy)
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Cache-Control', 'no-store')
        if packed:
            self.send_header('Content-Encoding', 'gzip')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == '/api/device':
            details = dict(
                version=CONFIG['projectVersion'],
                git='SIMULATED CARD',
                chip='SIMULATED',
                battery_mv=4012,
                battery_percent=87,
                reader=self.reader,
                active_slot=self.active,
                busy=bool(self.session),
                settings=self.settings,
            )
            self.respond(json.dumps(details).encode(), 'application/json')
            return

        if self.path.startswith('/api/slot/'):
            slot = self.path.rsplit('/', 1)[1]
            if slot not in list('01234567'):
                self.respond(b'', status=404)
                return

            row = self.bank[int(slot)]
            hf = row['hf']
            lf = row['lf']
            hf_type = hf['image'][1:3] if hf else bytes(2)
            lf_type = lf['image'][1:3] if lf else bytes(2)
            data = hf_type + lf_type
            data += bytes([bool(hf and hf['enabled']) | (bool(lf and lf['enabled']) << 1)])
            for place in (hf, lf):
                name = place['name'].encode() if place else b''
                data += bytes([len(name)]) + name
            data += self.revision(int(slot), 'hf') + self.revision(int(slot), 'lf')
            self.respond(data)
            return

        if self.path == '/api/status':
            self.respond(json.dumps({'git': 'SIMULATED CARD', 'busy': False}).encode(), 'application/json')
            return

        name = urlsplit(self.path).path.lstrip('/') or 'index.html'
        if name == 'storage-unavailable.js' and self.storage_unavailable:
            script = b"""
                for (const key of ['localStorage', 'indexedDB']) {
                    Object.defineProperty(window, key, {
                        get() {
                            throw new DOMException('Storage disabled for this test', 'SecurityError');
                        }
                    });
                }
            """
            self.respond(script, 'text/javascript')
            return

        path = (SITE / name).resolve()
        if not path.is_relative_to(SITE.resolve()) or not path.is_file():
            self.respond(b'', status=404)
            return

        types = {
            '.html': 'text/html',
            '.css': 'text/css',
            '.mjs': 'text/javascript',
            '.svg': 'image/svg+xml',
            '.wasm': 'application/wasm',
            '.txt': 'text/plain',
        }
        content = path.read_bytes()
        if name == 'index.html' and self.storage_unavailable:
            content = content.replace(
                b'<script type="module"',
                b'<script src="/storage-unavailable.js"></script><script type="module"',
                1,
            )
        self.respond(gzip.compress(content, mtime=0), types[path.suffix], packed=True)

    def do_POST(self):
        data = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        parsed = urlsplit(self.path)
        path = parsed.path
        cls = type(self)
        if path == '/test/classic-block' and len(data) == 17:
            cls.classic_memory[data[0]] = data[1:]
            self.respond(b'')
            return
        if path == '/test/reset':
            cls.settings = deepcopy(cls.initial_settings)
            cls.classic_memory = [block_bytes(block) for block in range(256)]
            cls.lf_memory = [0x00148041, 0xFF800000, 0x0034A800, 0, 0, 0, 0]
            cls.bank = deepcopy(cls.initial_bank)
            cls.session = 0
            cls.active = 0
            cls.reader = False
            cls.reader_session = False
            cls.staging = None
            cls.snapshot = None
            cls.tag_kind = data.decode() or 'classic'
            self.respond(b'')
            return

        if path == '/api/settings':
            if self.session:
                self.respond(b'', status=409)
                return

            keys = ['animation', 'press_a', 'press_b', 'long_a', 'long_b', 'sleep', 'long_ms', 'pairing']
            if len(data) >= 2 and data[0] < len(keys):
                self.settings[keys[data[0]]] = int.from_bytes(data[1:], 'big')
                self.respond(bytes([0, 0x68]))
            elif len(data) == 7 and data[0] == 8:
                self.respond(bytes([0, 0x68]))
            else:
                self.respond(b'', status=400)
            return

        if path in ('/api/session', '/api/slots/session'):
            if cls.session:
                self.respond(b'', status=409)
                return

            cls.next_session += 1
            cls.session = cls.next_session
            cls.reader_session = path == '/api/session'
            cls.previous_reader = cls.reader
            if cls.reader_session:
                cls.reader = True
            cls.staging = None
            cls.snapshot = None
            self.respond(json.dumps(dict(session=cls.session)).encode(), 'application/json')
            return

        if parse_qs(parsed.query).get('id') != [str(cls.session)] or not cls.session:
            self.respond(b'', status=409)
            return

        if path in ('/api/keepalive', '/api/end'):
            if path == '/api/end':
                if cls.reader_session:
                    cls.reader = cls.previous_reader
                cls.session = 0
                cls.staging = None
                cls.snapshot = None
            self.respond(b'')
            return

        if path.startswith('/api/slots/'):
            if cls.reader_session:
                self.respond(b'', status=409)
                return

            self.slot_action(path.rsplit('/', 1)[1], data)
            return

        if not path.startswith('/api/command/'):
            self.respond(b'', status=404)
            return

        command = int(path.rsplit('/', 1)[1])
        if not cls.reader_session:
            self.respond(b'', status=409)
            return

        response = b''
        status = 0
        if command == 2000:
            if cls.tag_kind == 'st25tb':
                status = 1
            elif cls.tag_kind == 'type2':
                response = bytes([7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0, 0])
            else:
                response = bytes([4]) + cls.classic_memory[0][:4] + bytes([4, 0, 8, 0])
        elif command == 2039:
            if len(data) not in (19, 20, 51, 52):
                status = 0x60
            else:
                operation, method, model, uid_length = data[:4]
                block = data[11]
                old = cls.classic_memory[block]
                trailer = block // 4 * 4 + 3 if block < 128 else block // 16 * 16 + 15
                key_offset = 0
                if data[12] == 2:
                    key_offset = 10
                key = cls.classic_memory[trailer][key_offset:key_offset + 6]
                key_b_readable = cls.classic_memory[trailer][6:9] == bytes([255, 7, 128])
                before_matches = old == data[19:35]
                observed = old
                if method == 3 and block == trailer:
                    before_matches = old[6:10] == data[25:29]
                    observed = bytes(6) + old[6:]
                checked = len(data) in (20, 52) and data[-1] == 1
                if checked and model == 6 and cls.tag_kind != 'classic2k':
                    status = 1
                elif data[4:4 + uid_length] not in (cls.classic_memory[0][:uid_length], bytes(uid_length)):
                    status = 1
                elif method == 3 and (data[13:19] != key or (data[12] == 2 and key_b_readable)):
                    status = 6
                elif operation == 2 and block == 0 and cls.tag_kind == 'fixed-classic':
                    status = 0x75
                elif operation in (0, 3):
                    response = bytes([4]) + cls.classic_memory[0][:4] + bytes(3) + bytes([4, 0, cls.classic_memory[0][5]]) + observed
                elif not before_matches:
                    status = 0x75
                elif operation == 2:
                    cls.classic_memory[block] = data[35:51]
        elif command == 2040:
            flags = 0 if cls.tag_kind in ('fixed-classic', 'classic2k') else 1
            response = bytes([0, flags, 0, 1, 0]) + bytes(6)
            if len(data) == 16 + 6 * data[14] and data[-1] == 1:
                ats = bytes.fromhex('0c75778002c1052f2f01bcd660d3') if cls.tag_kind == 'classic2k' else b''
                response += bytes([1, len(ats)]) + ats
        elif command == 2032:
            response = bytes([0, 1, 0])
        elif command == 2002:
            response = b'\x01'
        elif command == 2026:
            if data not in (b'\x00', b'\x01'):
                status = 0x60
        elif command == 2015:
            keys = [data[i : i + 6] for i in range(3, len(data), 6)]
            trailer = cls.classic_memory[data[0] // 4 * 4 + 3]
            expected_key = trailer[10:] if data[1] == 0x61 else trailer[:6]
            if expected_key in keys:
                response = b'\x01' + expected_key
            else:
                status = 6
        elif command == 2028:
            uid = bytes([4, 1, 2, 3, 4, 5, 6])
            expected_length = 11 if data and data[0] == 5 else 10
            if cls.tag_kind != 'type2' or len(data) != expected_length or data[2] != 7 or data[3:10] != uid:
                status = 1
            elif data[0] == 0:
                response = bytes([0, 4, 4, 2, 1, 0, 0x0F, 3])
            elif data[0] in (1, 5):
                pages = [bytes([page, 0, 0, 0]) for page in range(45)]
                pages[:4] = [
                    bytes([4, 1, 2, 0x8F]),
                    bytes([3, 4, 5, 6]),
                    bytes([4, 0x48, 0, 0]),
                    bytes([0xE1, 0x10, 0x12, 0]),
                ]
                pages[41:] = [bytes([4, 0, 0, 255]), bytes([0, 5, 0, 0]), bytes(4), bytes(4)]

                if data[0] == 5:
                    first_page = data[1]
                    last_page = data[10]
                    if last_page < first_page or last_page - first_page >= 8 or last_page >= 45:
                        status = 2
                    else:
                        response = b''.join(pages[first_page : last_page + 1])
                else:
                    response = b''.join(pages[(data[1] + i) % 45] for i in range(4))
            elif data[0] == 2:
                response = bytes([42]) * 32
            elif data[0] == 3 and data[1] == 2:
                response = bytes([1, 0, 0])
            else:
                status = 2
        elif command == 3033:
            status = 0x40
            response = bytes([1, 1, 127, 0]) + b''.join(word.to_bytes(4, 'big') for word in cls.lf_memory) + bytes(4)
            if len(data) >= 11 and data[1] in (1, 2):
                # Fixed simulated EM410x image; hardware codecs are tested in C.
                desired = [0x00148041, 0xFF806028, 0x0C825000]
                fingerprint = 2166136261
                for value in response:
                    fingerprint = ((fingerprint ^ value) * 16777619) & 0xFFFFFFFF
                same = cls.lf_memory[:3] == desired
                if data[1] == 2 and data[-4:] != fingerprint.to_bytes(4, 'big'):
                    status = 0x76
                elif data[1] == 2:
                    cls.lf_memory[:3] = desired
                    response = bytes([1, 1, 127, 1 if same else 2]) + b''.join(word.to_bytes(4, 'big') for word in cls.lf_memory) + bytes(4)
                else:
                    response = response[:3] + bytes([int(same)]) + response[4:]
                response += bytes([3]) + b''.join(word.to_bytes(4, 'big') for word in desired) + bytes(16) + fingerprint.to_bytes(4, 'big')
        elif command == 3000:
            status = 0x40
            response = bytes([0, 103, 1, 2, 3, 4, 5])
        elif command == 2304 and cls.tag_kind == 'st25tb':
            status = 1
        elif command == 2305 and cls.tag_kind == 'st25tb':
            response = cls.st25_uid + bytes([0x5A, 16, 4])
        elif command == 2307 and cls.tag_kind == 'st25tb':
            # Snapshot header, observed Chip ID/sysOTP, then one state/lock pair per block.
            response = bytes([1, 2, 0, 0]) + cls.st25_uid + bytes([0, 16, 4, 8, 0, 0, 0, 0x5A])
            response += b'\xff' * 4
            response += b''.join(bytes([1, 0, block, 1, 2, 3]) for block in range(16))
        elif command == 2008:
            response = cls.classic_memory[data[1]]
        elif command == 2034:
            block = data[9] * 4 + 3
            if len(data) not in (37, 38) or data[8] != 1 or block >= 64:
                status = 0x60
            elif data[1:5] != cls.classic_memory[0][:4] or data[17:21] != cls.classic_memory[block][6:10]:
                status = 0x70
            elif len(data) == 37:
                cls.classic_memory[block] = data[21:27] + data[33:37] + data[27:33]
        elif command == 2038:
            if len(data) not in (59, 60) or data[9] != 0:
                status = 0x60
            elif data[27:43] != cls.classic_memory[0]:
                status = 0x70
            elif len(data) == 60:
                response = cls.classic_memory[0]
            else:
                cls.classic_memory[0] = data[43:59]
                response = cls.classic_memory[0]
        elif command == 2030:
            model = 6 if cls.tag_kind == 'classic2k' else 1
            blocks = 128 if model == 6 else 64
            if (
                cls.tag_kind not in ('classic', 'fixed-classic', 'classic2k')
                or len(data) != 59
                or data[:9] != bytes([model, 4]) + cls.classic_memory[0][:4] + bytes(3)
                or data[9] == 0
                or data[9] >= blocks
                or data[9] % 4 == 3
            ):
                status = 0x60
            elif cls.classic_memory[data[9]] != data[27:43]:
                status = 0x70
            else:
                cls.classic_memory[data[9]] = data[43:59]
                response = cls.classic_memory[data[9]]
        elif command == 2027:
            if len(data) != 13 or data[8:] != bytes([4, 1, 2, 3, 4]) or data[1] >= 40:
                status = 0x60
            else:
                status, response = self.classic_sector(data[1], data[0], data[2:8])
        elif command == 2042:
            status, response = self.classic_batch(data)
        else:
            status = 0x60

        self.respond(status.to_bytes(2, 'big') + response)

    def classic_sector(self, sector, key_type, key):
        cls = type(self)
        first = sector * 4 if sector < 32 else 128 + (sector - 32) * 16
        count = 4 if sector < 32 else 16
        trailer = cls.classic_memory[first + count - 1]
        key_offset = 10 if key_type == 0x61 else 0
        key_b_readable = trailer[6:9] == bytes([255, 7, 128])
        if key != trailer[key_offset:key_offset + 6] or (key_type == 0x61 and key_b_readable):
            return 6, b''
        blocks = list(cls.classic_memory[first:first + count])
        blocks[-1] = bytes(6) + blocks[-1][6:]
        return 0, ((1 << count) - 1).to_bytes(2, 'big') + b''.join(blocks)

    def classic_batch(self, data):
        """Follow firmware/application/src/net/classic_batch.c: kind 0 checks one
        key per block, kind 1 reads sectors; records stop at a lost target."""
        cls = type(self)
        if len(data) < 8 or data[0] != 1 or data[1] > 1 or data[2] not in (4, 7, 10):
            return 0x60, b''
        kind, size = data[1], data[2]
        head = 4 + size
        count = data[head - 1] if len(data) >= head else 0
        entries = [data[head + 8 * i:head + 8 * i + 8] for i in range(count)]
        if (not 1 <= count <= 8 or len(data) != head + 8 * count
                or any(entry[1] not in (0x60, 0x61) or (kind and entry[0] >= 40) for entry in entries)):
            return 0x60, b''
        required = 3 + sum(4 + (2 + (4 if entry[0] < 32 else 16) * 16 if kind else 7)
                           for entry in entries)
        # The HTTP reply buffer reserves two bytes for the command status.
        if required > 510:
            return 0x60, b''
        present = (cls.tag_kind in ('classic', 'fixed-classic', 'classic2k')
                   and data[3:head - 1] == cls.classic_memory[0][:4])
        if kind == 0 and not present:
            return 1, b''
        output = bytearray([1, kind, count])
        for index, entry in enumerate(entries):
            block, key_type, key = entry[0], entry[1], bytes(entry[2:8])
            if kind == 1:
                status, result = self.classic_sector(block, key_type, key) if present else (1, b'')
            else:
                trailer = cls.classic_memory[block // 4 * 4 + 3 if block < 128 else block // 16 * 16 + 15]
                expected = trailer[10:] if key_type == 0x61 else trailer[:6]
                key_b_readable = trailer[6:9] == bytes([255, 7, 128])
                authenticated = key == expected and not (key_type == 0x61 and key_b_readable)
                status, result = (0, b'\x01' + key) if authenticated else (6, bytes(7))
            output += status.to_bytes(2, 'big') + len(result).to_bytes(2, 'big') + result
            if status not in (0, 6):
                output[2] = index + 1
                break
        return 0, bytes(output)

    def slot_action(self, operation, data):
        cls = type(self)
        try:
            if operation == 'read':
                offset = int.from_bytes(data[:2], 'big')
                count = int.from_bytes(data[2:], 'big')
                assert (
                    cls.snapshot is not None
                    and len(data) == 4
                    and 0 < count <= 512
                    and offset + count <= len(cls.snapshot)
                )
                self.respond(cls.snapshot[offset : offset + count])
                return

            if operation == 'write':
                offset = int.from_bytes(data[:2], 'big')
                assert cls.staging and len(data) > 2 and offset == len(cls.staging['bytes'])
                cls.staging['bytes'].extend(data[2:])
                assert len(cls.staging['bytes']) <= cls.staging['size']
                self.respond(len(cls.staging['bytes']).to_bytes(2, 'big'))
                return

            if operation == 'commit':
                stage = cls.staging
                assert stage and len(stage['bytes']) == stage['size'] and len(data) == data[1] + 2
                if stage['revision'] != cls.revision(stage['slot'], stage['sense']):
                    cls.staging = None
                    self.respond(b'', status=409)
                    return

                cls.bank[stage['slot']][stage['sense']] = dict(
                    image=bytes(stage['bytes']), name=data[2:].decode(), enabled=bool(data[0])
                )
                cls.staging = None
            elif operation == 'activate':
                assert len(data) == 9 and data[0] < 8
                if data[1:5] != cls.revision(data[0], 'hf') or data[5:9] != cls.revision(data[0], 'lf'):
                    self.respond(b'', status=409)
                    return

                cls.active = data[0]
                cls.reader = False
            else:
                assert len(data) >= 2 and data[0] < 8 and data[1] in (1, 2)
                slot = data[0]
                sense = 'hf' if data[1] == 2 else 'lf'
                place = cls.bank[slot][sense]
                if operation == 'open':
                    assert len(data) in (2, 6)
                    if len(data) == 6 and data[2:6] != cls.revision(slot, sense):
                        self.respond(b'', status=409)
                        return

                    assert place
                    cls.snapshot = place['image']
                    self.respond(len(cls.snapshot).to_bytes(2, 'big') + cls.revision(slot, sense))
                    return

                if operation == 'begin':
                    assert len(data) == 8
                    if data[4:8] != cls.revision(slot, sense):
                        self.respond(b'', status=409)
                        return

                    cls.staging = dict(
                        slot=slot,
                        sense=sense,
                        size=int.from_bytes(data[2:4], 'big'),
                        revision=data[4:8],
                        bytes=bytearray(),
                    )
                    self.respond(b'')
                    return

                if len(data) < 6 or data[2:6] != cls.revision(slot, sense):
                    self.respond(b'', status=409)
                    return

                data = data[:2] + data[6:]
                if operation == 'clear':
                    cls.bank[slot][sense] = None
                elif operation == 'enable':
                    assert place and len(data) == 3
                    place['enabled'] = bool(data[2])
                elif operation == 'nick':
                    assert place and 0 < len(data[2:]) <= 32
                    place['name'] = data[2:].decode()
                else:
                    self.respond(b'', status=404)
                    return

            self.respond(bytes([0, 0x68]))
        except (AssertionError, IndexError, UnicodeDecodeError):
            self.respond(b'', status=400)


if __name__ == '__main__':
    if not SITE.is_dir():
        raise SystemExit('Run resource/tools/build_web.py first')

    parser = argparse.ArgumentParser()
    parser.add_argument('--card', choices=['classic', 'type2', 'st25tb'], default='classic')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--offline', action='store_true', help='Block external connections through CSP')
    parser.add_argument(
        '--storage-unavailable', action='store_true', help='Make browser storage access throw SecurityError'
    )

    args = parser.parse_args()
    SimulatedCard.tag_kind = args.card
    SimulatedCard.offline = args.offline
    SimulatedCard.storage_unavailable = args.storage_unavailable

    print(f'SIMULATED CARD: http://127.0.0.1:{args.port}', flush=True)
    ThreadingHTTPServer(('127.0.0.1', args.port), SimulatedCard).serve_forever()
