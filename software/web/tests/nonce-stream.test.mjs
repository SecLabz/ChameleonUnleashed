import test from 'node:test';
import assert from 'node:assert/strict';
import { createNonceStream } from '../src/device/nonce-stream.mjs';
import { nativeClassicJobRunner, setDeviceTransport } from '../src/device/transport.mjs';

function packet(start, count) {
  const data = new Uint8Array(count * 12);
  const view = new DataView(data.buffer);
  for (let i = 0; i < count; i++) {
    view.setUint32(i * 12, start + i);
  }
  return { status: 0, completed: start + count, data };
}

test('autonomous acquisition buffers ordered observations while the consumer is busy', async () => {
  const progress = [];
  const stream = createNonceStream({
    budget: 48,
    onProgress: (n) => progress.push(n),
    run: async (_, __, { onEvent }) => {
      for (let i = 0; i < 48; i += 16) {
        onEvent(packet(i, 16));
      }
      return { cancelled: false };
    }
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(stream.buffered, 48);
  const records = [...(await stream.read()), ...(await stream.read()), ...(await stream.read())];
  assert.deepEqual(
    records.map((r) => r.nt1),
    Array.from({ length: 48 }, (_, i) => i)
  );
  assert.deepEqual(progress, [16, 32, 48]);
  assert.deepEqual(await stream.read(), []);
  await stream.stop();
});

test('Stop waits for the in-flight batch and terminal acknowledgement', async () => {
  let finish;
  let started;
  const ready = new Promise((resolve) => {
    started = resolve;
  });
  const stream = createNonceStream({
    budget: 32,
    run: async (_, __, { signal, onEvent }) => {
      onEvent(packet(0, 16));
      started();
      await new Promise((resolve) => {
        finish = resolve;
      });
      assert(signal.aborted);
      onEvent(packet(16, 16));
      return { cancelled: true };
    }
  });
  await ready;
  let stopped = false;
  const stopping = stream.stop().then(() => {
    stopped = true;
  });
  await Promise.resolve();
  assert.equal(stopped, false);
  finish();
  await stopping;
  assert.equal(stream.drain().length, 32);
});

for (const [name, event, message] of [
  ['count gap', packet(16, 16), /count/],
  ['overflow', packet(0, 16), /count/],
  ['RF failure', { status: 6 }, /Authentication rejected/],
  [
    'invalid parity',
    (() => {
      const p = packet(0, 1);
      p.data[8] = 2;
      return p;
    })(),
    /parity/
  ]
]) {
  test(`acquisition rejects ${name}`, async () => {
    const stream = createNonceStream({
      budget: 8,
      run: async (_, __, { onEvent }) => {
        onEvent(event);
        return { cancelled: false };
      }
    });
    await assert.rejects(stream.read(), message);
    await assert.rejects(stream.stop(), message);
  });
}

test('already cancelled acquisition does not start a radio job', async () => {
  const controller = new AbortController();
  controller.abort();
  const stream = createNonceStream({ signal: controller.signal, run: () => assert.fail('started') });
  await assert.rejects(stream.read(), /cancelled/);
  await assert.rejects(stream.stop(), /cancelled/);
});

test('an early terminal event cannot masquerade as a complete acquisition', async () => {
  const stream = createNonceStream({
    budget: 32,
    run: async (_, __, { onEvent }) => {
      onEvent(packet(0, 16));
      return { cancelled: false };
    }
  });
  await new Promise((resolve) => setImmediate(resolve));
  await assert.rejects(stream.stop(), /Incomplete/);
  assert.equal(stream.drain().length, 16);
});

test('native runner stays bound to BLE after a transport change', async () => {
  const backend = {
    capabilities: { classicJobs: true },
    async classicJob() {
      assert.equal(this, backend);
      throw new Error('BLE disconnected');
    }
  };
  try {
    setDeviceTransport(backend);
    const run = nativeClassicJobRunner();
    setDeviceTransport(null);
    await assert.rejects(run(), /BLE disconnected/);
    assert.equal(nativeClassicJobRunner(), null);
    setDeviceTransport({ capabilities: {} });
    assert.equal(nativeClassicJobRunner(), null);
  } finally {
    setDeviceTransport(null);
  }
});
