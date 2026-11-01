import test from 'node:test';
import assert from 'node:assert/strict';
import {
  applyHfDefaults,
  hfImage,
  hfDocument,
  blankNfcv,
  captureDocument,
  restoreHfDocument
} from '../src/documents/hf-image.mjs';
import { encodeSlotImage, decodeSlotImage } from '../src/documents/slot-image.mjs';
import { scanNfc, scanNfcA, scanNfcV, decodeSnapshot, readHf } from '../src/device/hf-reader.mjs';
import { serializeDraft, restoreDraft } from '../../../.cache/web/assets/modules/documents/workspace.mjs';

const absent = () => Object.assign(new Error('No tag'), { status: 1 });
const uid = [0xe0, 4, 1, 1, 2, 3, 4, 5];

const card = () => ({
  protocol: 'nfc_v',
  uid: [...uid],
  blocks: 28,
  block_size: 4,
  dsfid: 12,
  afi: null,
  ic_ref: null
});

function withCrc(data) {
  let value = 0xffff;
  for (const byte of data) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      value = (value >>> 1) ^ (value & 1 ? 0x8408 : 0);
    }
  }
  value ^= 0xffff;

  return Uint8Array.from([...data, value & 255, value >>> 8]);
}

function snapshot(complete = false, identity = card()) {
  const { uid, blocks } = identity;
  const data = new Uint8Array(24 + blocks * 6);
  const partialFlag = complete ? 0 : 1;
  const deviceStatus = complete ? 0 : 2;
  data.set([1, 3, partialFlag, deviceStatus, ...uid, 0, blocks, 4]);
  for (let i = 0; i < blocks; i++) {
    data.set([Number(complete || i === 0), i === 0 ? 2 : 0, 1, 2, 3, i], 24 + i * 6);
  }

  return data;
}

for (const type of [1500, 1501]) {
  const identity = {
    ...card(),
    uid: [0xe0, 4, type === 1500 ? 3 : 1, 0x10, 2, 3, 4, 5],
    blocks: type === 1500 ? 8 : 28
  };

  test(`new ${type} documents preserve unknown memory and first-read intent across reload`, async () => {
    const document = blankNfcv(type, identity.uid);
    assert.equal(document.type, type);
    assert.equal(document.unbound, true);
    assert.deepEqual(document.memory, Array(identity.blocks).fill(null));
    assert.deepEqual(document.locks, Array(identity.blocks).fill(null));
    assert.equal(document.emulator, undefined);
    assert.deepEqual(await restoreDraft(serializeDraft(document)), document);
    assert.throws(() => blankNfcv(type, [1, 2, 3, 4]), /UID/);
    assert.throws(() => blankNfcv(type, uid), /UID/);
  });

  test(`first ${type} read adopts the physical UID and retains partial results`, async () => {
    const document = blankNfcv(type, [...identity.uid.slice(0, 7), 99]);
    document.name = 'My tag';
    const calls = [];
    const password = type === 1500 ? [1, 2, 3, 4] : null;
    let current = document;
    const device = {
      nfcvPassword: password,
      async command(command, payload) {
        calls.push([command, payload]);
        if (command === 2300 || command === 2312) {
          return withCrc([0, 12, ...[...identity.uid].reverse()]);
        }
        assert.equal(command, 2301);
        return withCrc([0, 4, ...[...identity.uid].reverse(), identity.blocks - 1, 3]);
      },
      async dump(command, payload) {
        calls.push([command, payload]);
        return { status: 2, data: snapshot(false, identity) };
      }
    };
    const result = await readHf(device, document, (next) => {
      current = next;
    });
    assert.equal(current, result);
    assert.equal(result.name, 'My tag');
    assert.deepEqual(result.card.uid, identity.uid);
    assert.equal(result.unbound, undefined);
    assert.deepEqual(result.memory[0], [1, 2, 3, 0]);
    assert.equal(result.memory[1], null);
    assert.equal(result.capture.complete, false);
    assert.deepEqual(calls, [
      [password ? 2312 : 2300, password || []],
      [2301, identity.uid],
      [2308, [...identity.uid, 0, identity.blocks, 4]]
    ]);
    const restored = await restoreDraft(serializeDraft(result));
    assert.equal(restored.unbound, undefined);
    // A later read stays addressed to the adopted UID, including after reload.
    calls.length = 0;
    await readHf(device, restored, () => {});
    assert.deepEqual(calls, [[2308, [...identity.uid, 0, identity.blocks, 4]]]);
  });
}

test('first SLIX read rejects a different model without replacing the draft or dumping memory', async () => {
  const document = blankNfcv(1500, [0xe0, 4, 3, 0, 0, 0, 0, 1]);
  const original = serializeDraft(document);
  await assert.rejects(
    readHf(
      {
        async command(command) {
          if (command === 2300) {
            return withCrc([0, 12, ...[...uid].reverse()]);
          }
          return withCrc([0, 4, ...[...uid].reverse(), 27, 3]);
        },
        async dump() {
          assert.fail('A different model must not be read');
        }
      },
      document,
      () => assert.fail('A different model must not replace the draft')
    ),
    /not an ICODE SLIX-L/
  );
  assert.equal(serializeDraft(document), original);
});

test('a failed initial privacy authentication preserves the unbound draft without retry', async () => {
  const document = blankNfcv(1500, [0xe0, 4, 3, 0, 0, 0, 0, 1]);
  const original = serializeDraft(document);
  let attempts = 0;
  await assert.rejects(
    readHf(
      {
        nfcvPassword: [1, 2, 3, 4],
        async command(command) {
          assert.equal(command, 2312);
          attempts++;
          throw Object.assign(new Error('Authentication rejected'), { status: 6 });
        }
      },
      document,
      () => assert.fail('Authentication failure must not replace the draft')
    ),
    /Authentication rejected/
  );
  assert.equal(attempts, 1);
  assert.equal(serializeDraft(document), original);
});

test('NFC-A fast path sends no other family command', async () => {
  const calls = [];
  const device = {
    async command(command) {
      calls.push(command);

      return Uint8Array.from([4, 1, 2, 3, 4, 4, 0, 8, 0]);
    }
  };

  assert.equal((await scanNfc(device)).sak, 8);
  assert.deepEqual(calls, [2000]);

  await scanNfcA(device);

  assert.deepEqual(calls, [2000, 2000]);
});

test('NFC-A polling is opt-in and full discovery retains the empty request', async () => {
  const payloads = [];
  const device = {
    async command(command, payload) {
      assert.equal(command, 2000);

      payloads.push(payload);

      return Uint8Array.from([4, 1, 2, 3, 4, 4, 0, 8, 0]);
    }
  };
  await scanNfc(device, () => {}, true);
  await scanNfc(device);
  await scanNfcA(device);

  assert.deepEqual(payloads, [[1], [], []]);
});

test('missing NFC tags are retryable even with NFC-A-only firmware', async () => {
  for (const legacy of [false, true]) {
    await assert.rejects(
      scanNfc({
        async command(command) {
          if (command !== 2000 && legacy) {
            throw Object.assign(new Error('Unsupported'), { httpStatus: 404 });
          }
          throw absent();
        }
      }),
      (error) => error.status === 1
    );
  }
});

test('CRC failure is not absence and does not probe more families', async () => {
  const calls = [];

  await assert.rejects(
    scanNfc({
      async command(command) {
        calls.push(command);
        if (command === 2000) {
          throw absent();
        }

        return new Uint8Array(14);
      }
    }),
    /ATQB/
  );
  assert.deepEqual(calls, [2000, 2304]);
});

test('Inventory survives unsupported optional system information', async () => {
  const calls = [];
  const result = await scanNfc({
    async command(command) {
      calls.push(command);
      if (command === 2300) {
        return withCrc([0, 12, ...[...uid].reverse()]);
      }
      if (command === 2301) {
        throw Object.assign(new Error('Unsupported'), { httpStatus: 404 });
      }
      throw absent();
    }
  });

  assert.deepEqual(result.uid, uid);
  assert.equal(result.dsfid, 12);
  assert.equal(result.blocks, null);
  assert.deepEqual(calls, [2000, 2304, 2305, 2300, 2301]);
});

test('one dump retains returned partial blocks after cancellation', async () => {
  const controller = new AbortController();
  const calls = [];
  const document = hfDocument(card());

  await readHf(
    {
      signal: controller.signal,

      async dump(command, payload) {
        calls.push([command, payload]);
        controller.abort();

        return { status: 2, data: snapshot() };
      }
    },
    document,
    () => {}
  );

  assert.equal(calls.length, 1);
  assert.equal(calls[0][0], 2308);
  assert.deepEqual(calls[0][1], [...uid, 0, 28, 4]);
  assert.deepEqual(document.memory[0], [1, 2, 3, 0]);
  assert.equal(document.memory[1], null);
  assert.equal(document.locks[0], true);
  assert.equal(document.capture.dsfid, 12);
  assert.equal(document.capture.complete, false);

  const restored = restoreHfDocument(JSON.parse(JSON.stringify(document)));

  assert.deepEqual(restored.memory, document.memory);
  assert.deepEqual(captureDocument(document.capture).locks, document.locks);
});

test('snapshots reject wrong identity, false completeness and invalid lock states', () => {
  const data = snapshot();
  data[4] ^= 1;

  assert.throws(() => decodeSnapshot(data, card()), /snapshot/);

  const falseComplete = snapshot();
  falseComplete[2] = 0;
  falseComplete[3] = 0;

  assert.throws(() => decodeSnapshot(falseComplete, card()), /Incomplete/);

  const badLock = snapshot();
  badLock[25] = 3;

  assert.throws(() => decodeSnapshot(badLock, card()), /state/);
  assert.equal(decodeSnapshot(snapshot(true), card()).complete, true);
});

test('NFC-B PUPI is an identity, not an NFC-A UID', () => {
  const document = hfDocument({
    protocol: 'nfc_b',
    pupi: [1, 2, 3, 4],
    application: [0, 0, 0, 0],
    protocol_info: [0, 0, 0]
  });

  assert.equal(document.card.uid, undefined);
  assert.equal(document.memory.length, 0);
  assert.deepEqual(restoreHfDocument(document), document);
});

test('unsupported NFC-V geometry remains an identity through draft reload', () => {
  const document = hfDocument({ ...card(), blocks: 32, block_size: 8 });

  assert.equal(document.memory.length, 0);
  assert.equal(restoreHfDocument(document).card.block_size, 8);
});

test('ST25TB slot round trip preserves explicit UID, system and chip ID', () => {
  const document = hfDocument({
    protocol: 'st25tb',
    uid: [0xd0, 2, 0x33, 1, 2, 3, 4, 5],
    blocks: 16,
    block_size: 4,
    chip_id: 0x5a
  });
  document.memory = Array.from({ length: 16 }, (_, i) => [i, 1, 2, 3]);

  assert.throws(() => encodeSlotImage(document), /system/);

  document.card.system = [255, 255, 255, 255];

  const wire = encodeSlotImage(document);

  assert.equal(wire.length, 82);

  const restored = decodeSlotImage(wire);

  assert.deepEqual(restored.card.uid, document.card.uid);
  assert.deepEqual(restored.memory, document.memory);
  assert.equal(restored.card.chip_id, 0x5a);
  assert.deepEqual(encodeSlotImage(restored), wire);
  assert.deepEqual(restoreHfDocument(restored).memory, document.memory);
});

test('ST25TB reread replaces a missing or chosen Chip ID with its observation', async () => {
  const uid = [0xd0, 2, 0x33, 5, 4, 3, 2, 1];
  const document = hfDocument({ protocol: 'st25tb', uid, blocks: 16, block_size: 4, chip_id: null });
  const data = new Uint8Array(24 + 16 * 6);
  data.set([1, 2, 0, 0, ...uid, 0, 16, 4, 8]);
  data[19] = 0x42;
  data.fill(255, 20, 24);
  for (let i = 0; i < 16; i++) {
    data.set([1, 0, 1, 2, 3, 4], 24 + 6 * i);
  }

  await readHf(
    {
      async dump() {
        return { status: 0, data };
      }
    },
    document,
    () => {}
  );

  assert.equal(document.card.chip_id, 0x42);
  assert.equal(encodeSlotImage(document)[81], 0x42);
});

test('SRI512 capture preserves its identity without selecting the AT emulator or exporter', async () => {
  for (const product of [0x18, 0x19, 0x1a, 0x1b]) {
    const uid = [0xd0, 2, product, 5, 4, 3, 2, 1];
    const document = hfDocument({ protocol: 'st25tb', uid, blocks: 16, block_size: 4, chip_id: 0x5a });

    assert.match(document.geometry.name, /^SRI512/);
    assert.equal(document.type, undefined);

    const data = new Uint8Array(120);
    data.set([1, 2, 0, 0, ...uid, 0, 16, 4, 8]);
    data[19] = 0x5a;
    data.fill(255, 20, 24);
    for (let i = 0; i < 16; i++) {
      data.set([1, 0, i, i, i, i], 24 + 6 * i);
    }

    await readHf(
      {
        async dump(command, payload) {
          assert.equal(command, 2307);
          assert.deepEqual(payload, uid);

          return { status: 0, data };
        }
      },
      document,
      () => {}
    );

    assert.equal(document.capture.complete, true);

    const restored = captureDocument(JSON.parse(JSON.stringify(document.capture)));

    assert.deepEqual(restored.memory, document.memory);
    assert.deepEqual(restoreHfDocument(restored).card.uid, uid);
    assert.throws(() => encodeSlotImage(restored), /Unsupported|ST25TB save/);
    assert.throws(() => hfImage(restored), /use JSON/);
  }
});

test('NFC-V export overlays newly observed metadata on explicit defaults', async () => {
  const document = hfDocument({ ...card(), dsfid: 0, afi: null, ic_ref: null });
  applyHfDefaults(document);

  const data = snapshot(true);
  data[15] = 7;
  data.set([12, 23, 34], 16);
  await readHf(
    {
      async dump() {
        return { status: 0, data };
      }
    },
    document,
    () => {}
  );

  const image = hfImage(document);
  assert.deepEqual(image.metadata.slice(5, 8), [12, 23, 34]);
});

test('privacy scan binds the unlocked SLIX-L identity without persisting the password', async () => {
  const uid = [0xe0, 4, 3, 1, 2, 3, 4, 5];
  const password = [1, 2, 3, 4];
  const calls = [];
  const device = {
    async command(command, payload) {
      calls.push([command, [...payload]]);
      if (command === 2312) {
        return withCrc([0, 0, ...uid.toReversed()]);
      }
      assert.equal(command, 2301);
      return withCrc([0, 4, ...uid.toReversed(), 7, 3]);
    }
  };
  const card = await scanNfcV(device, password);
  assert.equal(card.blocks, 8);
  assert.deepEqual(card.uid, uid);
  assert.deepEqual(calls, [
    [2312, password],
    [2301, uid]
  ]);
  assert.equal(JSON.stringify(card).includes('password'), false);
  uid[2] = 1;
  await assert.rejects(scanNfcV(device, password), /not an ICODE SLIX-L/);
});
