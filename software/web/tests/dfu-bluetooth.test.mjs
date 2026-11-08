import test from 'node:test';
import assert from 'node:assert/strict';
import { DfuBluetooth, connectDfu, crc32, NORDIC_DFU } from '../src/device/dfu-bluetooth.mjs';

const image = {
  packet: Uint8Array.of(0x12, 0xc0, 0xdb, 0, 0xff),
  payload: Uint8Array.from({ length: 9001 }, (_, i) => i & 255)
};
const u32 = (value) => {
  const result = new Uint8Array(4);
  new DataView(result.buffer).setUint32(0, value, true);
  return [...result];
};
function bootloader({
  version,
  unsupportedVersion,
  maximumPacket = 20,
  truncate,
  badProbeCrc,
  queuedWrites,
  erasePending,
  dropFinalReply,
  badCrc,
  rejectCommand,
  staleData,
  disconnectAt,
  abort,
  timeoutMs = 100
} = {}) {
  const device = new EventTarget();
  device.gatt = {
    connected: true,
    disconnect() {
      this.connected = false;
      device.dispatchEvent(new Event('gattserverdisconnected'));
    }
  };
  const control = new EventTarget();
  const commands = [];
  const packets = [];
  const windows = [];
  const committed = [];
  const buffers = { 1: [], 2: [] };
  let type;
  let prn = 0;
  let remaining = 0;
  let receiptBytes = 0;
  let queue = Promise.resolve();
  let erasing = false;
  let queued = 0;
  let receipts = 0;
  let objectStart = 0;
  const fast = version >= 0x43550002 && version >>> 16 === 0x4355;
  control.startNotifications = async () => {};
  control.writeValueWithResponse = async (request) => {
    commands.push([...request]);
    const opcode = request[0];
    let payload = [];
    let status = 1;
    if (opcode === 11) {
      status = unsupportedVersion ? 2 : 1;
      payload = version === undefined ? [] : [2, ...u32(version), ...u32(0xf3000), ...u32(0xb000)];
    } else if (opcode === 2) {
      prn = request[1] + request[2] * 256;
      remaining = prn;
      receiptBytes = 0;
    } else if (opcode === 6) {
      payload = [
        ...u32(request[1] === 1 ? 512 : 4096),
        ...u32(staleData && request[1] === 2 ? 4 : 0),
        ...u32(0)
      ];
    } else if (opcode === 1) {
      type = request[1];
      remaining = prn;
      receiptBytes = 0;
      if (type === 1) {
        buffers[1] = [];
        buffers[2] = [];
      }
      objectStart = buffers[type].length;
      queued = 0;
      if (type === 2 && erasePending && !fast) {
        erasing = true;
        queued = 0;
        receipts = 0;
      }
    } else if (opcode === 3) {
      payload = [...u32(buffers[type].length), ...u32(crc32(buffers[type]) ^ (badCrc && type === 2 ? 1 : 0))];
    } else if (opcode === 4) {
      if (type === 1 && rejectCommand) {
        status = 0x0b;
      } else {
        committed.push({ type, bytes: buffers[type].slice(objectStart) });
      }
    }
    control.value = new DataView(Uint8Array.of(0x60, opcode, status, ...payload).buffer);
    // Nordic notifications can arrive before the browser's write promise settles.
    const final = opcode === 4 && type === 2 && buffers[2].length === image.payload.length;
    if (!dropFinalReply || !final) {
      control.dispatchEvent(new Event('characteristicvaluechanged'));
    }
    if (opcode === 4 && type === 2 && buffers[2].length === image.payload.length) {
      queueMicrotask(() => device.gatt.disconnect());
    }
  };
  const consume = (bytes) => {
    // The SDK drops writes when its 16-entry flash queue (including ERASE)
    // fills. Receipts still count received packets, not completed flash writes.
    if (type !== 2 || (fast ? queued++ < 17 : !erasing || queued++ < 15)) {
      buffers[type].push(...bytes);
    }
    if (type === 2 && buffers[2].length >= disconnectAt) {
      device.gatt.disconnect();
    }
    abort?.(type, buffers[type].length);
    receiptBytes += bytes.length;
    if (prn && --remaining === 0) {
      windows.push(receiptBytes);
      receiptBytes = 0;
      remaining = prn;
      const crc = crc32(buffers[type]) ^ ((badCrc && type === 2) || (badProbeCrc && type === 1) ? 1 : 0);
      if (fast) {
        queued = 0; // New receipts release only completed flash writes.
      }
      control.value = new DataView(
        Uint8Array.of(0x60, 3, 1, ...u32(buffers[type].length), ...u32(crc)).buffer
      );
      control.dispatchEvent(new Event('characteristicvaluechanged'));
      if (type === 2 && ++receipts >= 2) {
        erasing = false;
      }
    }
  };
  const packet = {
    async writeValueWithoutResponse(bytes) {
      packets.push([...bytes]);
      if (bytes.length > maximumPacket) {
        if (!truncate) {
          throw new DOMException('Packet exceeds ATT MTU', 'InvalidModificationError');
        }
        bytes = bytes.subarray(0, maximumPacket);
      }
      if (queuedWrites) {
        queue = queue.then(
          () =>
            new Promise((resolve) =>
              setImmediate(() => {
                consume(bytes);
                resolve();
              })
            )
        );
      } else {
        consume(bytes);
      }
    }
  };
  const session = new DfuBluetooth(device, control, packet, { timeoutMs });
  return { session, device, control, packet, commands, packets, windows, committed, buffers };
}

test('IEEE CRC-32 agrees with the standard vector and supports incremental windows', () => {
  const bytes = new TextEncoder().encode('123456789');
  assert.equal(crc32(bytes), 0xcbf43926);
  assert.equal(crc32(bytes.subarray(4), crc32(bytes.subarray(0, 4))), 0xcbf43926);
});

test('Secure DFU writes raw GATT packets, validates CRCs and activates each object once', async (t) => {
  const target = bootloader();
  t.after(() => target.session.close());
  const progress = [];
  await target.session.flash(image, (value) => progress.push(value));
  assert.deepEqual(target.commands[0], [11, 0]);
  assert.deepEqual(target.commands[1], [6, 1]);
  assert.deepEqual(target.commands[2], [1, 1, 5, 0, 0, 0]);
  assert.deepEqual(target.commands[3], [2, 1, 0]);
  assert.equal(
    target.commands.some(([opcode]) => opcode === 3),
    false
  );
  assert.deepEqual(target.buffers[1], [...image.packet]);
  assert.deepEqual(target.buffers[2], [...image.payload]);
  assert.deepEqual(
    target.committed.map(({ type, bytes }) => [type, bytes.length]),
    [
      [1, 5],
      [2, 4096],
      [2, 4096],
      [2, 809]
    ]
  );
  assert.equal(
    target.packets.every((bytes) => bytes.length <= 20),
    true
  );
  assert.deepEqual(progress, [4096 / 9001, 8192 / 9001, 1]);
  assert.equal(target.session.resetObserved, true);
});

for (const fault of [{ badCrc: true }, { rejectCommand: true }, { staleData: true }, { disconnectAt: 40 }]) {
  test(`DFU stops before activating firmware after ${JSON.stringify(fault)}`, async (t) => {
    const target = bootloader(fault);
    t.after(() => target.session.close());
    await assert.rejects(target.session.flash(image));
    assert.equal(
      target.committed.some(({ type }) => type === 2),
      false
    );
  });
}

test('cancellation after a completed transfer window sends no execute or extra packet', async (t) => {
  const abort = new AbortController();
  const target = bootloader();
  t.after(() => target.session.close());
  target.session.signal = abort.signal;
  await assert.rejects(
    target.session.flash(image, () => abort.abort()),
    { name: 'AbortError' }
  );
  assert.equal(target.buffers[2].length, 4096);
  assert.equal(
    target.committed.some(({ type }) => type === 2),
    false
  );
});

test('malformed, unexpected, rejected and missing notifications never trigger retries', async (t) => {
  for (const reply of [[0x60, 6, 1], [0x60, 2, 1, 0], [0x60, 2, 8], null]) {
    const target = bootloader({ timeoutMs: 5 });
    t.after(() => target.session.close());
    target.control.writeValueWithResponse = async () => {
      if (reply) {
        target.control.value = new DataView(Uint8Array.from(reply).buffer);
        target.control.dispatchEvent(new Event('characteristicvaluechanged'));
      }
    };
    await assert.rejects(target.session.flash(image));
    assert.equal(target.packets.length, 0);
  }
});

test('DFU chooser requests the bootloader service and uses its distinct identity', async (t) => {
  const target = bootloader();
  t.after(() => target.session.close());
  const chosen = [];
  const characteristics = [];
  target.device.gatt.connect = async () => ({
    async getPrimaryService(uuid) {
      assert.equal(uuid, NORDIC_DFU.service);
      return {
        async getCharacteristic(uuid) {
          characteristics.push(uuid);
          return uuid === NORDIC_DFU.control ? target.control : target.packet;
        }
      };
    }
  });
  const session = await connectDfu({
    bluetooth: {
      async requestDevice(options) {
        chosen.push(options);
        return target.device;
      }
    }
  });
  t.after(() => session.close());
  assert.deepEqual(chosen, [{ filters: [{ services: [0xfe59], namePrefix: 'CU-' }] }]);
  assert.deepEqual(characteristics, [NORDIC_DFU.control, NORDIC_DFU.packet]);
});

test('a reset without the final execute acknowledgement is never reported as success', async (t) => {
  const target = bootloader({ dropFinalReply: true });
  t.after(() => target.session.close());
  await assert.rejects(target.session.flash(image), /disconnected/);
});

test('cancelling the native DFU chooser never connects a device selected later', async () => {
  const stop = new AbortController();
  let choose;
  let connections = 0;
  const device = {
    gatt: {
      connect() {
        connections++;
      }
    }
  };
  const connecting = connectDfu({
    signal: stop.signal,
    bluetooth: {
      requestDevice() {
        return new Promise((resolve) => {
          choose = resolve;
        });
      }
    }
  });
  stop.abort();
  await assert.rejects(connecting, { name: 'AbortError' });
  choose(device);
  await new Promise(setImmediate);
  assert.equal(connections, 0);
});

test('receiver receipts gate each window when native write promises resolve before transmission', async (t) => {
  const target = bootloader({ queuedWrites: true });
  t.after(() => target.session.close());
  await target.session.flash(image);
  assert.deepEqual(target.buffers[2], [...image.payload]);
  assert.equal(
    target.commands.some(([opcode]) => opcode === 3),
    false
  );
});

test('initial page-erasure backpressure does not overflow the Nordic flash queue', async (t) => {
  const target = bootloader({ erasePending: true });
  t.after(() => target.session.close());
  await target.session.flash(image);
  assert.deepEqual(target.buffers[2], [...image.payload]);
});

const largeInit = { ...image, packet: Uint8Array.from({ length: 144 }, (_, i) => i) };
for (const mode of [
  { maximumPacket: 244, queuedWrites: true, erasePending: true },
  { maximumPacket: 20 },
  { maximumPacket: 20, truncate: true },
  { maximumPacket: 101, truncate: true }
]) {
  test(`RAM-only negotiation verifies usable packet size: ${JSON.stringify(mode)}`, async (t) => {
    const target = bootloader(mode);
    t.after(() => target.session.close());
    await target.session.flash(largeInit);
    assert.equal(target.session.packetBytes, mode.maximumPacket - (mode.maximumPacket % 4));
    assert.deepEqual(target.buffers[1], [...largeInit.packet]);
    assert.deepEqual(target.buffers[2], [...largeInit.payload]);
    assert.equal(target.committed.filter(({ type }) => type === 1).length, 1);
    assert.equal(target.committed.find(({ type }) => type === 1).bytes.length, largeInit.packet.length);
    if (mode.maximumPacket === 244) {
      assert.ok(target.packets.length < 50, 'Large writes reduce native calls by an order of magnitude');
      assert.ok(
        Math.max(...target.windows) <= 244,
        'Large packets must not enlarge unacknowledged flash windows'
      );
    }
  });
}

test('a corrupt probe stops before validating the init command or writing firmware', async (t) => {
  const target = bootloader({ maximumPacket: 244, badProbeCrc: true });
  t.after(() => target.session.close());
  await assert.rejects(target.session.flash(largeInit), /packet-size verification/);
  assert.equal(target.committed.length, 0);
  assert.equal(target.buffers[2].length, 0);
});

test('an unknown native probe failure never retries or starts firmware writes', async (t) => {
  const target = bootloader();
  t.after(() => target.session.close());
  target.packet.writeValueWithoutResponse = async () => {
    throw new DOMException('Lost link', 'NetworkError');
  };
  await assert.rejects(target.session.flash(largeInit), { name: 'NetworkError' });
  assert.equal(target.committed.length, 0);
  assert.equal(target.buffers[2].length, 0);
});

for (const maximumPacket of [244, 180, 100, 20]) {
  test(`revision 2 batches stay within the flash queue at MTU payload ${maximumPacket}`, async (t) => {
    const target = bootloader({ version: 0x43550002, maximumPacket, truncate: true, queuedWrites: true });
    t.after(() => target.session.close());
    await target.session.flash(largeInit);
    assert.equal(target.session.fast, true);
    assert.equal(target.session.packetBytes, maximumPacket === 244 ? 244 : Math.min(maximumPacket, 128));
    assert.deepEqual(target.buffers[2], [...image.payload]);
    assert.ok(Math.max(...target.windows) <= 17 * target.session.packetBytes);
    if (maximumPacket >= 128) {
      const receipts = target.commands.filter(([op]) => op === 2);
      const expected =
        maximumPacket === 244
          ? [
              [2, 1, 0],
              [2, 17, 0],
              [2, 4, 0]
            ]
          : [
              [2, 1, 0],
              [2, 2, 0],
              [2, 16, 0],
              [2, 7, 0]
            ];
      assert.deepEqual(receipts, expected);
    }
  });
}
for (const mode of [{ version: 0x43550001 }, { version: 0x58580002 }, { unsupportedVersion: true }]) {
  test(`unknown and older bootloaders retain conservative windows: ${JSON.stringify(mode)}`, async (t) => {
    const target = bootloader({ ...mode, maximumPacket: 244, erasePending: true });
    t.after(() => target.session.close());
    await target.session.flash(largeInit);
    assert.equal(target.session.fast, false);
    assert.ok(Math.max(...target.windows) <= 244);
    assert.deepEqual(target.buffers[2], [...image.payload]);
  });
}
