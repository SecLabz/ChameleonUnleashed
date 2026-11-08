"""PM3 stop channel held open before simulation, qualified for hf 14a sim.

No serial open, termios change or CLI startup is performed by stop(). The
supervisor owns this descriptor independently of the simulation's CLI process.
Another simulation family still needs its own physical stop preflight.
"""
from contextlib import contextmanager
import os
import secrets
import select
import signal
import struct
import termios
import time


@contextmanager
def opening_deadline(seconds):
    def expired(_signal, _frame):
        raise TimeoutError('Opening the PM3 stop channel exceeded its deadline')
    previous = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


def command_frame(command, data=b''):
    return struct.pack('<IHH', 0x61334d50, 0x8000 | len(data), command) + data + b'a3'


def crc_a(data):
    crc = 0x6363
    for value in data:
        value ^= crc & 0xff
        value ^= (value << 4) & 0xff
        crc = ((crc >> 8) ^ (value << 8) ^ (value << 3) ^ (value >> 4)) & 0xffff
    # PM3 stores (first CRC byte << 8 | second byte) in a little-endian field.
    return crc.to_bytes(2, 'big')


class StopChannel:
    def __init__(self, port):
        self.fd = None
        self.received = bytearray()
        self.pending = bytearray()
        try:
            with opening_deadline(3):
                self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                settings = termios.tcgetattr(self.fd)
                settings[0] = termios.IGNPAR
                settings[1] = 0
                settings[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
                settings[3] = 0
                settings[4] = settings[5] = termios.B115200
                settings[6][termios.VMIN] = settings[6][termios.VTIME] = 0
                termios.tcsetattr(self.fd, termios.TCSANOW, settings)
            self.ping(timeout=2)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def _write(self, data, deadline):
        while data:
            if time.monotonic() >= deadline:
                raise TimeoutError('PM3 stop channel write timed out')
            if select.select([], [self.fd], [], 0.05)[1]:
                try:
                    count = os.write(self.fd, data)
                except BlockingIOError:
                    continue
                if not count:
                    raise ConnectionError('PM3 stop channel closed while writing')
                data = data[count:]

    def _matching_ping(self, nonce):
        while True:
            start = self.pending.find(b'PM3b')
            if start < 0:
                self.pending[:] = self.pending[-3:]
                return False
            del self.pending[:start]
            if len(self.pending) < 10:
                return False
            _, length, status, _reason, command = struct.unpack_from('<IHbbH', self.pending)
            size = length & 0x7fff
            if not length & 0x8000 or size > 512:
                del self.pending[0]
                continue
            total = 12 + size
            if len(self.pending) < total:
                return False
            frame = bytes(self.pending[:total])
            if frame[-2:] not in (b'b3', crc_a(frame[:-2])):
                del self.pending[0]
                continue
            del self.pending[:total]
            # Successful replies normally carry an undefined reason (-1) on
            # the physical PM3. Only status, command and fresh ping data decide success.
            if command == 0x0109 and status == 0 and frame[10:-2] == nonce:
                return True

    def ping(self, timeout, interrupt=False):
        if self.fd is None:
            raise ConnectionError('PM3 stop channel is closed')
        nonce = secrets.token_bytes(16)
        deadline = time.monotonic() + timeout
        if interrupt:
            self._write(command_frame(0x0118), deadline)
            # Keep the unacknowledged BREAK and following PING in distinct USB
            # transfers. This PM3 can strand the second NG frame when both are
            # coalesced into one CDC packet while leaving its simulation loop.
            time.sleep(min(0.05, timeout / 4))
        self._write(command_frame(0x0109, nonce), deadline)
        while time.monotonic() < deadline:
            if select.select([self.fd], [], [], min(0.05, max(0, deadline - time.monotonic())))[0]:
                try:
                    data = os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                if not data:
                    raise ConnectionError('PM3 stop channel closed while reading')
                self.received.extend(data)
                self.pending.extend(data)
                if self._matching_ping(nonce):
                    return
        raise TimeoutError('PM3 stop channel did not receive the matching ping')

    def stop(self, timeout=5):
        self.ping(timeout, interrupt=True)
