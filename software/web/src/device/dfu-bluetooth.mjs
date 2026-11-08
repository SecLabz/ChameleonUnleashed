// Nordic Secure DFU GATT transport. Control replies and cumulative CRCs gate
// every object; an interrupted operation is never automatically replayed.
import { claimBluetoothDevice } from './bluetooth-lock.mjs';

export const NORDIC_DFU = {
  service: 0xfe59,
  control: '8ec90001-f315-4f60-9fb8-838830daea50',
  packet: '8ec90002-f315-4f60-9fb8-838830daea50'
};
const OP = { CREATE: 1, PRN: 2, CRC: 3, EXECUTE: 4, SELECT: 6, VERSION: 11, RESPONSE: 0x60 };
const OBJECT = { COMMAND: 1, DATA: 2 };
const PACKET_BYTES = 20; // ATT's mandatory 23-byte MTU minus its 3-byte header.
const MAX_PACKET_BYTES = 244; // Bootloader MTU 247 minus the ATT header.
const WINDOW_PACKETS = 12; // Leave headroom in Nordic's 16-entry flash queue.
const WINDOW_BYTES = PACKET_BYTES * WINDOW_PACKETS;
const FAST_PACKET_BYTES = 128; // Divides a 4 KiB object, keeping PRN stable between pages.
const FAST_WINDOW_PACKETS = 16; // One spare BLE buffer; full-size packets fill all 17 slots.
const VERSION_REPLY_BYTES = 13; // Type byte, then version, address and size as uint32.
const BOOTLOADER_TYPE = 2;
const BOOTLOADER_FAMILY = 0x4355; // "CU", matching bootloader_version.py.
const FAST_DFU_REVISION = 2;
const REQUEST_TIMEOUT_MS = 10000;
const RESET_TIMEOUT_MS = 30000;
const MAX_OBJECT_BYTES = 1024 * 1024;
const CRC_POLYNOMIAL = 0xedb88320; // Reflected IEEE CRC-32, as used by Nordic DFU.
const CRC_TABLE = Uint32Array.from({ length: 256 }, (_, value) => {
  for (let bit = 0; bit < 8; bit++) {
    value = (value >>> 1) ^ (value & 1 ? CRC_POLYNOMIAL : 0);
  }
  return value >>> 0;
});
export function crc32(bytes, previous = 0) {
  let crc = previous ^ 0xffffffff;
  for (const byte of bytes) {
    crc = (crc >>> 8) ^ CRC_TABLE[(crc ^ byte) & 255];
  }
  return (crc ^ 0xffffffff) >>> 0;
}
const number = (bytes, offset = 0) =>
  new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true);
const little = (value) => {
  const bytes = new Uint8Array(4);
  new DataView(bytes.buffer).setUint32(0, value, true);
  return bytes;
};

export class DfuBluetooth {
  constructor(device, control, packet, { signal, timeoutMs = REQUEST_TIMEOUT_MS } = {}) {
    this.device = device;
    this.control = control;
    this.packet = packet;
    this.signal = signal;
    this.timeoutMs = timeoutMs;
    this.packetBytes = PACKET_BYTES;
    this.fast = false;
    this.pending = null;
    this.closed = false;
    this.expectReset = false;
    this.resetObserved = false;
    this.failure = new Promise((_, reject) => {
      this.reject = reject;
    });
    this.failure.catch(() => {});
    this.abort = () => this.fail(signal.reason);
    this.disconnected = () => {
      this.resetObserved = true;
      if (!this.expectReset || (this.pending && !this.pending.responded)) {
        this.fail(
          new Error('Bluetooth DFU disconnected. Reconnect in DFU and start the installation again.')
        );
      }
      this.reset?.();
    };
    this.received = (event) => {
      try {
        const value = event.target.value;
        const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        const pending = this.pending;
        if (
          !pending ||
          pending.responded ||
          bytes.length < 3 ||
          bytes[0] !== OP.RESPONSE ||
          bytes[1] !== pending.opcode
        ) {
          throw new Error('Unexpected Bluetooth DFU reply.');
        }
        const unsupportedVersion = pending.opcode === OP.VERSION && bytes[2] === 2 && bytes.length === 3;
        if (bytes[2] !== 1 && !unsupportedVersion) {
          throw new Error(`Bluetooth DFU rejected opcode ${pending.opcode} (status ${bytes[2]}).`);
        }
        if (pending.size !== null && bytes.length !== pending.size + 3) {
          throw new Error('Invalid Bluetooth DFU reply length.');
        }
        pending.responded = true;
        pending.resolve(bytes.slice(3));
      } catch (error) {
        this.fail(error);
      }
    };
    device.addEventListener('gattserverdisconnected', this.disconnected);
    control.addEventListener('characteristicvaluechanged', this.received);
    signal?.addEventListener('abort', this.abort, { once: true });
  }

  fail(error) {
    this.closed = true;
    this.reject(error);
  }

  async wait(operation, timeout = this.timeoutMs) {
    this.signal?.throwIfAborted();
    if (this.closed) {
      throw new Error('Bluetooth DFU connection is closed.');
    }
    let timer;
    try {
      return await Promise.race([
        Promise.resolve().then(operation),
        this.failure,
        new Promise((_, reject) => {
          timer = setTimeout(
            () => reject(new Error('Bluetooth DFU timed out. Reconnect and start again.')),
            timeout
          );
        })
      ]);
    } catch (error) {
      this.fail(error);
      throw error;
    } finally {
      clearTimeout(timer);
    }
  }

  async exchange(opcode, size, write) {
    if (this.pending) {
      throw new Error('Bluetooth DFU request already in progress.');
    }
    let resolve;
    const response = new Promise((yes) => {
      resolve = yes;
    });
    this.pending = { opcode, size, resolve, responded: false };
    try {
      const [reply] = await this.wait(() => Promise.all([response, write()]));
      return reply;
    } finally {
      this.pending = null;
    }
  }

  request(opcode, payload = [], size = 0) {
    return this.exchange(opcode, size, () =>
      this.control.writeValueWithResponse(Uint8Array.of(opcode, ...payload))
    );
  }

  async select(type) {
    const reply = await this.request(OP.SELECT, [type], 12);
    const maximum = number(reply);
    if (!maximum || maximum > MAX_OBJECT_BYTES) {
      throw new Error('Invalid Bluetooth DFU object size.');
    }
    return { maximum, offset: number(reply, 4), crc: number(reply, 8) };
  }

  async write(bytes, offset, crc) {
    for (let position = 0; position < bytes.length; ) {
      // Older bootloaders acknowledge CREATE before erasure and PRN before writing.
      let packets = this.packetBytes === MAX_PACKET_BYTES ? FAST_WINDOW_PACKETS + 1 : FAST_WINDOW_PACKETS;
      if (!this.fast) {
        packets = position < WINDOW_BYTES ? 1 : Math.max(1, Math.floor(WINDOW_BYTES / this.packetBytes));
      }
      const window = bytes.subarray(position, position + this.packetBytes * packets);
      const count = Math.ceil(window.length / this.packetBytes);
      if (this.prn !== count) {
        await this.request(OP.PRN, [count, 0]);
        this.prn = count;
      }
      // Write promises can complete before the controller sends their packets.
      // Wait for the receiver's PRN instead of letting a CRC read overtake them.
      const reply = await this.exchange(OP.CRC, 8, async () => {
        for (let packet = 0; packet < window.length; packet += this.packetBytes) {
          await this.wait(() =>
            this.packet.writeValueWithoutResponse(window.subarray(packet, packet + this.packetBytes))
          );
        }
      });
      position += window.length;
      offset += window.length;
      crc = crc32(window, crc);
      if (number(reply) !== offset || number(reply, 4) !== crc) {
        throw new Error('Bluetooth DFU offset/CRC mismatch. No further object will be activated.');
      }
    }
    return { offset, crc };
  }

  async negotiate(packet, maximum) {
    if (packet.length <= PACKET_BYTES || maximum < MAX_PACKET_BYTES) {
      return;
    }
    // Probe only the RAM init buffer, never flash or EXECUTE. Some platforms
    // silently truncate writes: the receiver's offset AND CRC determine capacity.
    const probe = new Uint8Array(MAX_PACKET_BYTES);
    probe.set(packet.subarray(0, probe.length));
    await this.request(OP.CREATE, [OBJECT.COMMAND, ...little(probe.length)]);
    await this.request(OP.PRN, [1, 0]);
    this.prn = 1;
    const reply = await this.exchange(OP.CRC, 8, async () => {
      try {
        await this.packet.writeValueWithoutResponse(probe);
      } catch (error) {
        if (error.name !== 'InvalidModificationError') {
          throw error;
        }
        this.pending.resolve(null); // Rejected length; no write was accepted.
      }
    });
    if (reply) {
      const accepted = number(reply);
      if (
        accepted < PACKET_BYTES ||
        accepted > probe.length ||
        number(reply, 4) !== crc32(probe.subarray(0, accepted))
      ) {
        throw new Error('Bluetooth DFU packet-size verification failed.');
      }
      this.packetBytes = accepted - (accepted % 4); // Nordic flash writes must be word-aligned.
    }
  }

  async flash({ packet, payload }, onProgress = () => {}) {
    this.signal?.throwIfAborted();
    if (!packet?.length || !payload?.length) {
      throw new Error('Empty Nordic DFU image.');
    }
    await this.wait(() => this.control.startNotifications());
    const version = await this.request(OP.VERSION, [0], null);
    if (version.length !== 0 && version.length !== VERSION_REPLY_BYTES) {
      throw new Error('Invalid Bluetooth DFU firmware version reply.');
    }
    const encoded = version.length ? number(version, 1) : 0;
    this.fast =
      version[0] === BOOTLOADER_TYPE &&
      encoded >>> 16 === BOOTLOADER_FAMILY &&
      (encoded & 0xffff) >= FAST_DFU_REVISION;
    const command = await this.select(OBJECT.COMMAND);
    if (packet.length > command.maximum) {
      throw new Error('Nordic init packet exceeds the DFU limit.');
    }
    await this.negotiate(packet, command.maximum);
    if (this.fast && this.packetBytes >= FAST_PACKET_BYTES && this.packetBytes < MAX_PACKET_BYTES) {
      this.packetBytes = FAST_PACKET_BYTES;
    }
    await this.request(OP.CREATE, [OBJECT.COMMAND, ...little(packet.length)]);
    await this.write(packet, 0, 0);
    await this.request(OP.EXECUTE);
    const data = await this.select(OBJECT.DATA);
    if (data.offset || data.crc) {
      throw new Error('DFU did not reset the previous firmware transfer.');
    }
    let offset = 0;
    let crc = 0;
    while (offset < payload.length) {
      const chunk = payload.subarray(offset, offset + data.maximum);
      await this.request(OP.CREATE, [OBJECT.DATA, ...little(chunk.length)]);
      ({ offset, crc } = await this.write(chunk, offset, crc));
      this.expectReset = offset === payload.length;
      onProgress(
        offset / payload.length,
        this.expectReset ? 'Verifying and activating firmware…' : 'Installing firmware…'
      );
      await this.request(OP.EXECUTE);
    }
    if (!this.resetObserved) {
      await this.wait(
        () =>
          new Promise((resolve) => {
            this.reset = resolve;
          }),
        RESET_TIMEOUT_MS
      );
    }
  }

  close() {
    this.device.removeEventListener('gattserverdisconnected', this.disconnected);
    this.control.removeEventListener('characteristicvaluechanged', this.received);
    this.signal?.removeEventListener('abort', this.abort);
    this.fail(new Error('Bluetooth DFU connection closed.'));
    this.releaseLock?.();
    if (this.device.gatt.connected) {
      this.device.gatt.disconnect();
    }
  }
}

// Call directly from a click: the bootloader is a separate Bluetooth identity
// on platforms that increment its BLE address, so a second chooser is expected.
export async function connectDfu({
  bluetooth = globalThis.navigator?.bluetooth,
  locks = globalThis.navigator?.locks,
  signal
} = {}) {
  signal?.throwIfAborted();
  if (!bluetooth) {
    throw new Error('This browser does not support Web Bluetooth.');
  }
  let device;
  let session;
  let releaseLock;
  let timer;
  let abort;
  let abandoned = false;
  const cancelled = new Promise((_, reject) => {
    abort = () => reject(signal.reason);
    signal?.addEventListener('abort', abort, { once: true });
  });
  const step = async (promise) => {
    const result = await promise;
    if (abandoned || signal?.aborted) {
      device?.gatt.disconnect();
      throw new Error('Bluetooth DFU connection cancelled.');
    }
    return result;
  };
  try {
    device = await Promise.race([
      bluetooth.requestDevice({
        filters: [{ services: [NORDIC_DFU.service], namePrefix: 'CU-' }]
      }),
      cancelled
    ]);
    const setup = async () => {
      await step(
        claimBluetoothDevice(device, locks).then((release) => {
          releaseLock = release;
          if (abandoned) {
            release();
          }
        })
      );
      const server = await step(device.gatt.connect());
      const service = await step(server.getPrimaryService(NORDIC_DFU.service));
      const control = await step(service.getCharacteristic(NORDIC_DFU.control));
      const packet = await step(service.getCharacteristic(NORDIC_DFU.packet));
      session = new DfuBluetooth(device, control, packet, { signal });
      session.releaseLock = releaseLock;
      return session;
    };
    return await Promise.race([
      setup(),
      cancelled,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('Bluetooth DFU connection timed out.')), RESET_TIMEOUT_MS);
      })
    ]);
  } catch (error) {
    abandoned = true;
    session?.close();
    releaseLock?.();
    if (releaseLock && device?.gatt.connected) {
      device.gatt.disconnect();
    }
    throw error;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
  }
}
