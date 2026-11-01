import test from 'node:test';
import assert from 'node:assert/strict';
import { SlotDevice } from '../../../.cache/web/assets/modules/device/slots.mjs';
import { decodeSlotImage, encodeSlotImage, nickname } from '../src/documents/slot-image.mjs';

const originalFetch = globalThis.fetch;

function document(type = 1001) {
  return {
    name: 'Test',
    type,
    kind: 'classic',
    unitBytes: 16,
    card: { uid: [1, 2, 3, 4], atqa: [0, 4], sak: 8, ats: [1, 2, 3] },
    memory: Array.from({ length: type === 1003 ? 256 : 64 }, () => Array(16).fill(42)),
    emulator: { options: [3, 1, 1, 0, 1, 1, 2], extra: [] }
  };
}

function server({ abortAfterWrite, badReadback = false, failCommit = false, conflict = false } = {}) {
  const state = { stored: null, uploaded: [], commits: 0, ended: false, otherFrequency: [1, 2, 3, 4] };
  globalThis.fetch = async (url, { body }) => {
    const response = (bytes, type = 'application/octet-stream', status = 200) =>
      new Response(bytes, { status, headers: { 'Content-Type': type } });
    const operation = new URL(url, 'http://device').pathname.split('/').at(-1);
    const bytes = Array.from(body || []);
    if (operation === 'session') {
      return response(JSON.stringify({ session: 1 }), 'application/json');
    }

    assert(url.endsWith('id=1'));

    if (operation === 'begin') {
      assert.equal(bytes.length, 8);
      assert.deepEqual(bytes.slice(4), [1, 2, 3, 4]);

      if (conflict) {
        return response(new Uint8Array(), 'application/octet-stream', 409);
      }
      state.uploaded = [];

      return response(new Uint8Array());
    }

    if (operation === 'write') {
      assert.equal(bytes[0] * 256 + bytes[1], state.uploaded.length);

      state.uploaded.push(...bytes.slice(2));
      abortAfterWrite?.abort();

      return response(new Uint8Array([state.uploaded.length >> 8, state.uploaded.length & 255]));
    }

    if (operation === 'commit') {
      state.commits++;
      if (failCommit) {
        return response(new Uint8Array([0, 0x70]));
      }
      state.stored = new Uint8Array(state.uploaded);
      if (badReadback) {
        state.stored[state.stored.length - 1] ^= 1;
      }

      return response(new Uint8Array([0, 0x68]));
    }

    if (operation === 'open') {
      return response(new Uint8Array([state.stored.length >> 8, state.stored.length & 255, 1, 2, 3, 4]));
    }

    if (operation === 'read') {
      const offset = bytes[0] * 256 + bytes[1];
      const size = bytes[2] * 256 + bytes[3];

      return response(state.stored.slice(offset, offset + size));
    }

    if (operation === 'end') {
      state.ended = true;
      state.uploaded = [];

      return response(new Uint8Array());
    }

    assert.fail(`Unexpected request ${operation}`);
  };

  return state;
}

test('Classic 4K uploads use bounded ordered chunks and verify the complete stored image', async () => {
  const state = server();
  const device = new SlotDevice();

  try {
    await device.begin();

    const source = document(1003);
    const saved = await device.save(7, 2, source, 'Entrée', 0x01020304);

    assert.deepEqual(encodeSlotImage(saved), encodeSlotImage(source));
    assert.equal(state.commits, 1);
    assert.deepEqual(state.otherFrequency, [1, 2, 3, 4]);
  } finally {
    await device.end();
    globalThis.fetch = originalFetch;
  }

  assert(state.ended);
});

test('cancelled uploads never commit and release the reservation', async () => {
  const controller = new AbortController();
  const state = server({ abortAfterWrite: controller });
  const device = new SlotDevice(controller.signal);

  try {
    await device.begin();

    await assert.rejects(device.save(0, 2, document(), 'Test', 0x01020304), /cancelled/);
    assert.equal(state.commits, 0);
    assert.equal(state.stored, null);
  } finally {
    await device.end();
    globalThis.fetch = originalFetch;
  }

  assert(state.ended);
  assert.deepEqual(state.uploaded, []);
});

test('persistent-save errors and changed readback are never reported as success', async () => {
  for (const options of [{ failCommit: true }, { badReadback: true }]) {
    const state = server(options);
    const device = new SlotDevice();

    try {
      await device.begin();

      await assert.rejects(
        device.save(0, 2, document(), 'Test', 0x01020304),
        /unconfirmed|verification failed/
      );
    } finally {
      await device.end();
      globalThis.fetch = originalFetch;
    }

    assert(state.ended);
  }
});

test('stale saves are rejected before upload or commit', async () => {
  const state = server({ conflict: true });
  const device = new SlotDevice();

  try {
    await device.begin();

    await assert.rejects(device.save(0, 2, document(), 'Test', 0x01020304), /slot changed/);
    assert.equal(state.commits, 0);
    assert.deepEqual(state.uploaded, []);
  } finally {
    await device.end();
    globalThis.fetch = originalFetch;
  }
});

test('slot image round trips retain Type 2 metadata and LF data', () => {
  const type2 = {
    name: 'NTAG',
    type: 1100,
    unitBytes: 4,
    kind: 'type2',
    card: { uid: [4, 1, 2, 3, 4, 5, 6], atqa: [0, 0x44], sak: 0, ats: [] },
    memory: Array.from({ length: 45 }, () => [0, 1, 2, 3]),
    emulator: { options: [1, 3, 1, 0, 45], extra: Array.from({ length: 11 }, () => [5, 6, 7, 8]) }
  };
  const restored = decodeSlotImage(encodeSlotImage(type2));

  assert.deepEqual(restored.memory, type2.memory);
  assert.deepEqual(restored.emulator, type2.emulator);
  assert.deepEqual(restored.card, type2.card);

  const lf = {
    name: 'Viking',
    type: 170,
    unitBytes: 4,
    kind: 'lf',
    card: { uid: [], atqa: [], sak: 0, ats: [] },
    memory: [[1, 2, 3, 4]]
  };

  assert.deepEqual(decodeSlotImage(encodeSlotImage(lf)).memory, lf.memory);
  assert.throws(() => encodeSlotImage({ ...lf, memory: [null] }), /complete/);
  assert.throws(() => nickname('é'.repeat(17)), /32 UTF-8/);
});
