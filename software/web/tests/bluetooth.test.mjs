import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setImmediate as nextTick } from 'node:timers/promises';
import {
  BLE_API,
  OPERATION,
  FrameDecoder,
  encodeFrame,
  encodeOperation,
  encodeRequest,
  decodeReply
} from '../src/device/ble-protocol.mjs';
import { BluetoothTransport, connectBluetooth } from '../src/device/bluetooth.mjs';
import { DeviceSession, deviceTransportType, request, setDeviceTransport } from '../src/device/transport.mjs';

test('BLE operation IDs match the firmware contract and Chameleon framing', () => {
  const header = readFileSync(
    new URL('../../../firmware/application/src/net/ble_api.h', import.meta.url),
    'utf8'
  );
  const names = header
    .split('enum {')[1]
    .split('};')[0]
    .split(',')
    .map((name) => name.trim().replace('BLE_API_', ''));
  assert.deepEqual(names, Object.keys(OPERATION));
  assert.deepEqual([...encodeFrame(1000)], [0x11, 0xef, 3, 0xe8, 0, 0, 0, 0, 0x15, 0]);
  assert.deepEqual(
    [...encodeOperation(OPERATION.COMMAND, 0x12345678, 0x9abcdeff, 2015, Uint8Array.of(9))],
    [1, 0x12, 0x34, 0x56, 0x78, 8, 0x9a, 0xbc, 0xde, 0xff, 7, 0xdf, 9]
  );
});

test('BLE routes preserve GET/POST distinction, leases and all slot/job operations', () => {
  for (const [path, data, operation, argument] of [
    ['/api/device', undefined, OPERATION.DEVICE, 0],
    ['/api/status', undefined, OPERATION.STATUS, 0],
    ['/api/update', undefined, OPERATION.UPDATE_INFO, 0],
    ['/api/slot/7', undefined, OPERATION.SLOT, 7],
    ['/api/session', new Uint8Array(), OPERATION.READER_SESSION, 0],
    ['/api/slots/session', new Uint8Array(), OPERATION.SLOT_SESSION, 0],
    ['/api/settings', Uint8Array.of(0, 1), OPERATION.SETTINGS, 0],
    ['/api/command/2027?id=42', new Uint8Array(), OPERATION.COMMAND, 2027],
    ...['open', 'read', 'begin', 'write', 'commit', 'activate', 'nick', 'enable', 'clear'].map((name) => [
      `/api/slots/${name}?id=42`,
      new Uint8Array(),
      OPERATION[`SLOT_${name.toUpperCase()}`],
      0
    ]),
    ...['start', 'state', 'cancel'].map((name) => [
      `/api/st25tb/tearoff/${name}?id=42`,
      new Uint8Array(),
      OPERATION[`TEAROFF_${name.toUpperCase()}`],
      0
    ]),
    ...['keepalive', 'end'].map((name) => [
      `/api/${name}?id=42`,
      new Uint8Array(),
      OPERATION[name.toUpperCase()],
      0
    ])
  ]) {
    const bytes = encodeRequest(path, data, 7);
    const view = new DataView(bytes.buffer);
    assert.equal(bytes[5], operation, path);
    assert.equal(view.getUint16(10), argument);
    assert.equal(view.getUint32(6), path.includes('?id=') ? 42 : 0);
    assert.deepEqual(bytes.slice(12), data ?? new Uint8Array());
  }
  for (const [path, data] of [
    ['/api/session', undefined],
    ['/api/device', new Uint8Array()],
    ['/api/slot/8', undefined],
    ['/api/command/2000?id=0', new Uint8Array()],
    ['/api/end?id=4294967296', new Uint8Array()],
    ['/api/command/65536?id=1', new Uint8Array()],
    ['/api/end?id=1&other=2', new Uint8Array()],
    ['/api/update/enter', new Uint8Array()]
  ]) {
    assert.throws(() => encodeRequest(path, data, 1));
  }
  assert.throws(() => encodeRequest('/api/settings', new Uint8Array(1025), 1));
});

test('BLE frame decoder accepts every split, concatenation and DataView offset', () => {
  const bytes = encodeFrame(
    BLE_API.COMMAND,
    Uint8Array.from({ length: 520 }, (_, n) => n),
    0x68
  );
  for (let split = 0; split <= bytes.length; split++) {
    const parser = new FrameDecoder();
    const results = [...parser.push(bytes.slice(0, split)), ...parser.push(bytes.slice(split))];
    assert.equal(results.length, 1);
    assert.deepEqual(results[0].data, bytes.slice(9, -1));
  }
  const joined = new Uint8Array(bytes.length * 2 + 10);
  joined.set(bytes, 3);
  joined.set(bytes, bytes.length + 3);
  assert.equal(new FrameDecoder().push(new DataView(joined.buffer, 3, bytes.length * 2)).length, 2);
  for (const offset of [0, 8, 20, bytes.length - 1]) {
    const bad = bytes.slice();
    bad[offset] ^= 1;
    assert.throws(() => new FrameDecoder().push(bad));
  }
  assert.throws(() => new FrameDecoder().push(new Uint8Array(8213)));
});

function fakeGatt(t, respond, timeoutMs = 1000, withoutResponse = false, payloadLimit) {
  const device = new EventTarget();
  device.gatt = {
    connected: true,
    disconnect() {
      this.connected = false;
      device.dispatchEvent(new Event('gattserverdisconnected'));
    }
  };
  const tx = new EventTarget();
  tx.startNotifications = async () => {};
  const frames = [];
  const fragments = [];
  let writes = 0;
  let simultaneous = 0;
  const parser = new FrameDecoder();
  const rx = {
    async writeValueWithResponse(bytes) {
      simultaneous = Math.max(simultaneous, ++writes);
      fragments.push(bytes.slice());
      await nextTick();
      for (const frame of parser.push(bytes)) {
        frames.push(frame);
        const operation = frame.data[5];
        let reply;
        if (operation === OPERATION.HELLO) {
          reply = { status: 200, data: Uint8Array.of(1, 4, 0, 2, 0, payloadLimit === undefined ? 1 : 3) };
        } else if (operation === OPERATION.TRANSPORT_INFO && payloadLimit !== undefined) {
          reply = { status: 200, data: Uint8Array.of(1, payloadLimit >> 8, payloadLimit & 255) };
        } else {
          reply = respond(frame);
        }
        if (reply) {
          const body = reply.json ? new TextEncoder().encode(JSON.stringify(reply.data)) : reply.data;
          const payload = new Uint8Array(8 + body.length);
          payload[0] = 1;
          payload.set(frame.data.slice(1, 5), 1);
          payload[5] = reply.status >> 8;
          payload[6] = reply.status;
          payload[7] = +!!reply.json;
          payload.set(body, 8);
          if (reply.wrongCorrelation) {
            payload[4] ^= 1;
          }
          const response = encodeFrame(1090, payload, 0x68);
          for (let offset = 0; offset < response.length; offset += 7) {
            const part = response.slice(offset, offset + 7);
            const buffer = new Uint8Array(part.length + 5);
            buffer.set(part, 3);
            tx.value = new DataView(buffer.buffer, 3, part.length);
            tx.dispatchEvent(new Event('characteristicvaluechanged'));
          }
        }
      }
      writes--;
    }
  };
  if (withoutResponse) {
    rx.properties = { writeWithoutResponse: true };
    if (withoutResponse !== 'advertised-only') {
      rx.writeValueWithoutResponse = rx.writeValueWithResponse;
      rx.writeValueWithResponse = () => assert.fail('Unexpected acknowledged fragment');
    }
  }
  const backend = new BluetoothTransport(device, rx, tx, { timeoutMs });
  t.after(() => backend.disconnect());
  const notify = (bytes) => {
    tx.value = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    tx.dispatchEvent(new Event('characteristicvaluechanged'));
  };
  return { backend, device, frames, fragments, notify, maxWrites: () => simultaneous };
}

test('BLE requests serialize writes and reconstruct JSON and binary responses', async (t) => {
  const fake = fakeGatt(t, (frame) =>
    frame.data[5] === OPERATION.DEVICE
      ? { status: 200, json: true, data: { chip: 'test' } }
      : { status: 200, data: new Uint8Array(512).fill(42) }
  );
  await fake.backend.initialize();
  assert.deepEqual(fake.backend.capabilities, { tearoff: true, classicJobs: false });
  const [info, bytes] = await Promise.all([
    fake.backend.request('/api/device'),
    fake.backend.request('/api/command/2027?id=1', new Uint8Array(42))
  ]);
  assert.deepEqual(info, { chip: 'test' });
  assert.equal(bytes.length, 512);
  assert(bytes.every((byte) => byte === 42));
  assert.equal(fake.maxWrites(), 1);
  assert(fake.fragments.every((part) => part.length <= 20));
  assert.deepEqual(
    fake.frames.map((frame) => new DataView(frame.data.buffer).getUint32(1)),
    [1, 2, 3]
  );
});

test('BLE service errors retain the connection and HTTP-compatible status', async (t) => {
  const fake = fakeGatt(t, () => ({ status: 409, data: new Uint8Array() }));
  await fake.backend.initialize();
  await assert.rejects(
    fake.backend.request('/api/session', new Uint8Array()),
    (error) => error.httpStatus === 409
  );
  assert.equal(fake.device.gatt.connected, true);
});

function jobEvent(sequence, flags = 0, completed = 16) {
  return encodeFrame(
    BLE_API.JOB_EVENT,
    Uint8Array.from([
      1,
      0,
      0,
      0,
      7,
      sequence >> 8,
      sequence & 255,
      flags,
      2,
      0,
      0,
      completed >> 8,
      completed & 255,
      ...(flags & 2 ? [] : Array(192).fill(0))
    ]),
    0x68
  );
}

test('Classic streaming accepts events before the start write callback and serializes keepalive', async (t) => {
  let fake;
  fake = fakeGatt(t, (frame) => {
    if (frame.data[5] === OPERATION.CLASSIC_JOB_START) {
      queueMicrotask(() => {
        fake.notify(jobEvent(0));
        fake.notify(jobEvent(1, 1, 32));
      });
      return { status: 200, data: Uint8Array.of(0, 0, 0, 7) };
    }
    return { status: 200, data: new Uint8Array() };
  });
  await fake.backend.initialize();
  fake.backend.capabilities.classicJobs = true;
  const events = [];
  const [result] = await Promise.all([
    fake.backend.classicJob(42, [1, 2], { onEvent: (event) => events.push(event) }),
    fake.backend.request('/api/keepalive?id=42', new Uint8Array())
  ]);
  assert.equal(result.completed, 32);
  assert.equal(events.length, 2);
  assert.equal(fake.maxWrites(), 1);
  assert.equal(fake.backend.job, null);
  assert.equal(fake.backend.pending, null);
});

test('Classic streaming cancellation waits for its correlated response and terminal observations', async (t) => {
  const controller = new AbortController();
  let fake;
  fake = fakeGatt(t, (frame) => {
    if (frame.data[5] === OPERATION.CLASSIC_JOB_START) {
      queueMicrotask(() => fake.notify(jobEvent(0)));
      return { status: 200, data: Uint8Array.of(0, 0, 0, 7) };
    }
    assert.equal(frame.data[5], OPERATION.CLASSIC_JOB_CANCEL);
    queueMicrotask(() => fake.notify(jobEvent(1, 3)));
    return { status: 200, data: new Uint8Array() };
  });
  await fake.backend.initialize();
  fake.backend.capabilities.classicJobs = true;
  const events = [];
  const result = await fake.backend.classicJob(42, [1, 2], {
    signal: controller.signal,
    onEvent: (event) => {
      events.push(event);
      if (!event.done) {
        controller.abort();
      }
    }
  });
  assert.equal(result.cancelled, true);
  assert.equal(events[0].completed, 16);
  assert.equal(fake.frames.filter((f) => f.data[5] === OPERATION.CLASSIC_JOB_CANCEL).length, 1);
  assert.equal(fake.device.gatt.connected, true);
});

for (const scenario of ['sequence', 'disconnect', 'timeout']) {
  test(`a Classic stream ${scenario} fails without replay`, async (t) => {
    let fake;
    fake = fakeGatt(
      t,
      () => {
        queueMicrotask(() => {
          if (scenario === 'sequence') {
            fake.notify(jobEvent(1, 1));
          }
          if (scenario === 'disconnect') {
            fake.device.gatt.disconnect();
          }
        });
        return { status: 200, data: Uint8Array.of(0, 0, 0, 7) };
      },
      30
    );
    await fake.backend.initialize();
    fake.backend.capabilities.classicJobs = true;
    await assert.rejects(fake.backend.classicJob(42, [1, 2]));
    assert.equal(fake.frames.filter((f) => f.data[5] === OPERATION.CLASSIC_JOB_START).length, 1);
    assert.equal(fake.device.gatt.connected, false);
  });
}

for (const mode of [true, 'advertised-only']) {
  test(`fragment transmission preserves complete replies and serialization (without response: ${mode})`, async (t) => {
    const fake = fakeGatt(t, () => ({ status: 200, data: new Uint8Array(512).fill(37) }), 1000, mode);
    await fake.backend.initialize();
    const results = await Promise.all([
      fake.backend.request('/api/command/2027?id=1', new Uint8Array(257)),
      fake.backend.request('/api/command/2027?id=1', new Uint8Array(42))
    ]);
    assert.equal(fake.backend.writeWithoutResponse, mode === true);
    assert(results.every((value) => value.length === 512 && value.every((byte) => byte === 37)));
    assert.equal(fake.frames.length, 3);
    assert.equal(fake.maxWrites(), 1);
    assert(fake.fragments.every((fragment) => fragment.length <= 20));
  });
}

for (const limit of [20, 64, 244, 512]) {
  test(`the per-connection negotiated payload ${limit} bounds every native write`, async (t) => {
    const fake = fakeGatt(t, () => ({ status: 200, data: Uint8Array.of(7) }), 1000, true, limit);
    await fake.backend.initialize();
    assert.equal(fake.backend.fragmentBytes, limit);
    assert.deepEqual(
      await fake.backend.request('/api/command/2015?id=1', new Uint8Array(512)),
      Uint8Array.of(7)
    );
    assert(fake.fragments.every((fragment) => fragment.length <= limit));
    assert(fake.fragments.some((fragment) => fragment.length === limit));
    assert.equal(fake.frames.length, 3); // Hello, read-only negotiation, one command.
    assert.equal(fake.maxWrites(), 1);
  });
}

for (const limit of [0, 19, 513, 65535]) {
  test(`invalid negotiated payload ${limit} closes before application requests`, async (t) => {
    const fake = fakeGatt(
      t,
      () => assert.fail('No application request after invalid negotiation'),
      1000,
      true,
      limit
    );
    await assert.rejects(fake.backend.initialize(), /negotiated Bluetooth payload/);
    assert.equal(fake.device.gatt.connected, false);
    assert.equal(fake.frames.length, 2);
    assert(fake.fragments.every((fragment) => fragment.length <= 20));
  });
}

test('a lost reply invalidates the BLE connection and queued mutations are not replayed', async (t) => {
  const fake = fakeGatt(t, () => null, 20);
  await fake.backend.initialize();
  const results = await Promise.allSettled([
    fake.backend.request('/api/command/2030?id=1', Uint8Array.of(1)),
    fake.backend.request('/api/command/2030?id=1', Uint8Array.of(2))
  ]);
  assert.equal(results[0].status, 'rejected');
  assert.match(results[0].reason.message, /outcome may be unknown/);
  assert.equal(results[1].status, 'rejected');
  assert.equal(fake.device.gatt.connected, false);
  assert.equal(fake.frames.length, 2); // Handshake and one mutation only.
});

for (const [ending, withoutResponse] of [
  ['timeout', false],
  ['disconnect', false],
  ['timeout', true],
  ['disconnect', true]
]) {
  test(`a reply before a stalled native write completion permits ${ending} (without response: ${withoutResponse})`, async (t) => {
    const fake = fakeGatt(
      t,
      () => ({ status: 200, data: new Uint8Array() }),
      ending === 'timeout' ? 20 : 1000,
      withoutResponse
    );
    await fake.backend.initialize();
    const method = withoutResponse ? 'writeValueWithoutResponse' : 'writeValueWithResponse';
    const write = fake.backend.rx[method].bind(fake.backend.rx);
    let finishWrite;
    let signalStalled;
    const stalled = new Promise((resolve) => {
      signalStalled = resolve;
    });
    fake.backend.rx[method] = async (bytes) => {
      const completed = fake.frames.length;
      await write(bytes);
      if (fake.frames.length !== completed) {
        await new Promise((resolve) => {
          finishWrite = resolve;
          signalStalled();
        });
      }
    };
    const work = Promise.allSettled([
      fake.backend.request('/api/command/2030?id=1', Uint8Array.of(1)),
      fake.backend.request('/api/command/2030?id=1', Uint8Array.of(2))
    ]);
    let observer;
    try {
      await stalled; // The valid device reply has already been delivered.
      if (ending === 'disconnect') {
        fake.device.gatt.disconnect();
      }
      const result = await Promise.race([
        work,
        new Promise((resolve) => {
          observer = setTimeout(() => resolve('stuck'), 100);
        })
      ]);
      assert.notEqual(result, 'stuck', 'the full exchange must remain cancellable after its reply');
      assert(result.every((entry) => entry.status === 'rejected'));
      assert.match(
        result[0].reason.message,
        ending === 'timeout' ? /outcome may be unknown/ : /disconnected/
      );
      assert.equal(fake.device.gatt.connected, false);
      assert.equal(fake.frames.length, 2); // The queued mutation was never transmitted.
      finishWrite();
      await nextTick();
      assert.equal(fake.frames.length, 2); // A late native completion cannot replay it.
    } finally {
      clearTimeout(observer);
      fake.backend.disconnect();
      finishWrite?.();
      await work;
    }
  });
}

test('a mismatched response cannot complete the next BLE request', async (t) => {
  const fake = fakeGatt(t, () => ({ status: 200, data: new Uint8Array(), wrongCorrelation: true }));
  await fake.backend.initialize();
  await assert.rejects(fake.backend.request('/api/device'), /different request/);
  assert.equal(fake.device.gatt.connected, false);
});

test('existing reservation callers use the selected backend without HTTP fallback', async (t) => {
  const calls = [];
  t.mock.method(globalThis, 'fetch', () => assert.fail('Unexpected HTTP fallback'));
  setDeviceTransport({
    async request(path) {
      calls.push(path);
      return path.endsWith('/session') ? { session: 7 } : new Uint8Array();
    }
  });
  t.after(() => setDeviceTransport(null));
  assert.equal(deviceTransportType(), 'ble');
  const session = new DeviceSession();
  await session.reserve();
  await request('/api/device');
  await session.end();
  assert.deepEqual(calls, ['/api/session', '/api/device', '/api/end?id=7']);
});

test('BLE response contract rejects legacy firmware and oversized service replies', () => {
  assert.throws(() => decodeReply({ command: 1090, status: 0x67, data: new Uint8Array() }, 1), /Update/);
  assert.throws(() => decodeReply({ command: 1090, status: 0x68, data: new Uint8Array(521) }, 1));
});

test('a timed-out GATT connection releases the tab lock and disconnects a late success', async (t) => {
  const original = Object.getOwnPropertyDescriptor(globalThis, 'isSecureContext');
  Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
  t.after(() => {
    if (original) {
      Object.defineProperty(globalThis, 'isSecureContext', original);
    } else {
      delete globalThis.isSecureContext;
    }
  });
  let finishConnection;
  let lockReleased = false;
  let disconnects = 0;
  const device = {
    id: 'test',
    gatt: {
      connected: false,
      connect: () =>
        new Promise((resolve) => {
          finishConnection = resolve;
        }),
      disconnect() {
        this.connected = false;
        disconnects++;
      }
    }
  };
  await assert.rejects(
    connectBluetooth({
      bluetooth: { requestDevice: async () => device },
      locks: {
        async request(_key, _options, work) {
          await work({});
          lockReleased = true;
        }
      },
      connectTimeoutMs: 10
    }),
    /connection timed out/
  );
  await nextTick();
  assert.equal(lockReleased, true);
  device.gatt.connected = true;
  finishConnection({ getPrimaryService: () => assert.fail('Discovery started after timeout') });
  await nextTick();
  assert.equal(device.gatt.connected, false);
  assert.equal(disconnects, 1);
});

test('entering DFU sends the legacy reboot once and requires the actual disconnect', async (t) => {
  const device = new EventTarget();
  device.gatt = {
    connected: true,
    disconnect() {
      this.connected = false;
      device.dispatchEvent(new Event('gattserverdisconnected'));
    }
  };
  const frames = [];
  const rx = {
    async writeValueWithResponse(bytes) {
      frames.push(new FrameDecoder().push(bytes)[0]);
      queueMicrotask(() => device.gatt.disconnect());
      throw new Error('GATT reset before write callback');
    }
  };
  const tx = new EventTarget();
  const backend = new BluetoothTransport(device, rx, tx, { timeoutMs: 20 });
  t.after(() => backend.disconnect());
  await backend.enterDfu();
  assert.deepEqual(
    frames.map((frame) => [frame.command, frame.data.length]),
    [[1010, 0]]
  );
});
