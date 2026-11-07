import { withRuntime } from '../../../.cache/web/assets/modules/runtime/runtime.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { applyHfDefaults, needsHfDefaults, hfDocument } from '../src/documents/hf-image.mjs';

import { createEngine } from '../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';
import {
  blankClassic,
  blankType2,
  editBlock,
  exportClassic,
  exportNfc,
  importNfc,
  exportMct,
  importMct,
  inspectMct,
  knownBlock,
  memoryHex,
  parseMemoryHex,
  parseHex,
  restoreDraft,
  serializeDraft,
  applyType2Defaults,
  needsType2Defaults
} from '../../../.cache/web/assets/modules/documents/workspace.mjs';
import { Device } from '../../../.cache/web/assets/modules/device/device.mjs';
import {
  copyToTag,
  supportsPhysicalCopy,
  writeClassicChanges,
  writeType2Changes
} from '../../../.cache/web/assets/modules/device/tag-write.mjs';
import { classicChanges, classicReadSnapshot } from '../src/documents/classic-write.mjs';
import { readType2 } from '../../../.cache/web/assets/modules/device/type2-reader.mjs';
import { decodeSlotImage, encodeSlotImage } from '../src/documents/slot-image.mjs';
import { decodeSlot } from '../src/device/slots.mjs';
import { request } from '../src/device/transport.mjs';

const wasm = await readFile(
  new URL('../../../.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm', import.meta.url)
);

const originalFetch = globalThis.fetch;
const originalWorker = globalThis.Worker;
const workers = [];

class Worker {
  constructor() {
    workers.push(this);
  }

  postMessage({ id, wasm, request }) {
    queueMicrotask(async () => {
      try {
        if (wasm) {
          this.engine = await createEngine(wasm);
        }

        const value = wasm ? null : this.engine(request);
        if (!this.terminated) {
          this.onmessage({ data: { id, value } });
        }
      } catch (error) {
        if (!this.terminated) {
          this.onmessage({ data: { id, error: error.message } });
        }
      }
    });
  }

  terminate() {
    this.terminated = true;
  }
}

async function runtime(work) {
  globalThis.fetch = async () => new Response(wasm);
  globalThis.Worker = Worker;

  try {
    return await work();
  } finally {
    assert(workers.every((worker) => worker.terminated));

    workers.length = 0;
    globalThis.fetch = originalFetch;
    globalThis.Worker = originalWorker;
  }
}

test('shared physical planner distinguishes exact user data from skipped protected pages', () =>
  runtime(async () => {
    const memory = Array.from({ length: 45 }, () => [0, 0, 0, 0]);
    memory[41] = [0, 0, 0, 255];

    await withRuntime(async (call) => {
      const { writable } = await call({
        op: 'physical_permissions',
        model: 8,
        memory,
        key_b: [],
        authenticated: false
      });

      assert.equal(writable[4], true);

      const units = Array.from({ length: 36 }, (_, offset) => ({
        index: offset + 4,
        before: [0, 0, 0, 0],
        after: [0, 0, 0, 0],
        writable: writable[offset + 4]
      }));
      units[0].after = [1, 2, 3, 4];
      units[1].before = null;

      await assert.rejects(
        call({ op: 'physical_plan', model: 8, units, best_effort: false }),
        /cannot be copied/
      );

      const plan = await call({ op: 'physical_plan', model: 8, units, best_effort: true });

      assert.deepEqual(
        plan.writes.map((unit) => unit.index),
        [4]
      );
      assert.deepEqual(plan.skipped, [5]);

      memory[2][2] = 1 << 4;

      const locked = await call({
        op: 'physical_permissions',
        model: 8,
        memory,
        key_b: [],
        authenticated: false
      });

      assert.equal(locked.writable[4], false);
    });
  }));

for (const scenario of [
  'copy',
  'unprotected',
  'write protected',
  'no key',
  'wrong key',
  'changed target',
  'cancelled review',
  'lost write reply'
]) {
  test(`Ultralight C physical copy: ${scenario}`, (t) =>
    runtime(async () => {
      const memory = Array.from({ length: 48 }, () => [0, 0, 0, 0]);
      memory[40] = [2, 0, 0, 0];
      const protectedFrom = scenario === 'unprotected' ? 48 : 8;
      memory[42] = [protectedFrom, 0, 0, 0];
      memory[43] = [scenario === 'write protected' ? 1 : 0, 0, 0, 0];
      const card = { uid: [4, 1, 2, 3, 4, 5, 6], atqa: [0, 68], sak: 0, ats: [] };
      const source = {
        kind: 'type2',
        type: 1104,
        card,
        geometry: { pages: 48, profile: 'ultralight_c' },
        memory: structuredClone(memory)
      };
      source.memory[16] = [16, 0, 0, 0];
      source.memory[20] = [20, 0, 0, 0];
      const key = Array.from({ length: 16 }, (_, i) => i);
      const controller = new AbortController();
      let writes = 0;
      let prompts = 0;
      let confirmed = false;
      let ended = false;
      let reads = 0;
      t.mock.method(Device.prototype, 'begin', async function () {
        this.card = card;
        return card;
      });
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        assert.equal(command, 2031); // Never GET_VERSION for Ultralight C.
        assert.equal(payload.length, 27);
        assert.deepEqual(payload.slice(0, 9), [19, 7, ...card.uid]);
        const page = payload[9];
        assert(page < 44); // Secret key pages cannot be read.
        reads++;
        if (payload[10] === 4) {
          assert.deepEqual(payload.slice(11), key);
          if (scenario === 'wrong key') {
            throw Object.assign(new Error('Authentication rejected'), { status: 6 });
          }
        } else {
          assert.equal(payload[10], 0);
          assert.deepEqual(payload.slice(11), Array(16).fill(0));
          if (page >= protectedFrom && scenario !== 'write protected') {
            throw Object.assign(new Error('Protected'), { status: 2 });
          }
        }
        // The trailing pages deliberately wrap; they must never replace other pages.
        return Uint8Array.from([...memory[page], ...Array(12).fill(255)]);
      });
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        writes++;
        assert.equal(command, 2030);
        assert.equal(payload.length, 59);
        assert.deepEqual(payload.slice(0, 11), [19, 7, ...card.uid, 20, scenario === 'unprotected' ? 0 : 4]);
        assert.deepEqual(payload.slice(11, 27), scenario === 'unprotected' ? Array(16).fill(0) : key);
        assert.deepEqual(payload.slice(27, 31), memory[20]);
        assert.deepEqual(payload.slice(43, 47), [20, 0, 0, 0]);
        if (scenario === 'lost write reply') {
          throw new Error('Write outcome unknown');
        }
        return Uint8Array.from([20, 0, 0, 0]);
      });
      const options = {
        requestCredential: async (ultralightC) => {
          prompts++;
          assert(ultralightC);
          return scenario === 'no key' ? null : key;
        },
        confirmCopy: async ({ writes, omissions, clone }) => {
          confirmed = true;
          assert.equal(clone, null);
          assert.equal(writes, scenario === 'no key' ? 0 : 1);
          assert(omissions.some((value) => value.includes('protected')));
          if (scenario === 'cancelled review') {
            controller.abort();
          }
          if (scenario === 'changed target') {
            memory[40][0] = 0;
          }
          return writes > 0 ? 'data' : null;
        }
      };
      assert(supportsPhysicalCopy(source));
      const work = copyToTag(source, controller.signal, () => {}, options);
      if (scenario === 'wrong key') {
        await assert.rejects(work, /Authentication rejected/);
        assert.equal(confirmed, false);
      } else if (scenario === 'changed target') {
        await assert.rejects(work, /target changed/);
      } else if (scenario === 'cancelled review') {
        await assert.rejects(work, /cancelled/);
        assert.equal(reads, 88);
      } else if (scenario === 'lost write reply') {
        await assert.rejects(work, /outcome unknown/);
      } else {
        await work;
      }
      assert.equal(prompts, scenario === 'unprotected' ? 0 : 1);
      assert.equal(
        writes,
        ['copy', 'unprotected', 'write protected', 'lost write reply'].includes(scenario) ? 1 : 0
      );
      assert(ended);
      assert(reads >= 44);
    }));
}

test('all Classic blank documents export and reimport through shared codecs', () =>
  runtime(async () => {
    for (const [sak, count, atqa] of [
      [9, 20, [0, 4]],
      [8, 64, [0, 4]],
      [16, 128, [0, 4]],
      [24, 256, [0, 2]]
    ]) {
      const blank = await blankClassic(sak, [1, 2, 3, 4]);

      assert.equal(blank.memory.length, count);
      assert.deepEqual(blank.card.atqa, atqa);
      assert.deepEqual(blank.memory[0].slice(6, 8), [...atqa].reverse());

      const text = await exportClassic(blank);
      const imported = await importNfc(text, 'roundtrip');

      assert.deepEqual(imported.memory, blank.memory);
      assert.deepEqual(imported.card.uid, blank.card.uid);
    }
  }));

test('new Type 2 profiles survive NFC, browser draft and slot round trips', () =>
  runtime(async () => {
    const uid = [4, 1, 2, 3, 4, 5, 6];
    for (const [type, pages, capacity] of [
      [1100, 45, 0x12],
      [1101, 135, 0x3e],
      [1102, 231, 0x6d],
      [1103, 16, 0x06],
      [1104, 48, 0x12],
      [1105, 20, 0x06],
      [1106, 41, 0x10],
      [1107, 20, 0x06],
      [1108, 41, 0x10]
    ]) {
      const document = await blankType2(type, uid);
      assert.equal(document.memory.length, pages);
      assert.deepEqual(document.memory[0], [4, 1, 2, 0x8f]);
      assert.deepEqual(document.memory[1], [3, 4, 5, 6]);
      assert.deepEqual(document.memory[2], [4, 0x48, 0, 0]);
      assert.deepEqual(document.memory[3], [0xe1, 0x10, capacity, 0]);
      assert.equal(needsType2Defaults(document), false);
      assert.match(document.warnings.join(), /no original tag signature/);
      assert(document.memory.every(knownBlock));
      assert.deepEqual(decodeSlotImage(encodeSlotImage(document)).memory, document.memory);
      const restored = await restoreDraft(serializeDraft(document));
      assert.deepEqual(restored.memory, document.memory);
      assert.deepEqual(restored.emulator, document.emulator);
      const imported = await importNfc(await exportNfc(document), 'created');
      assert.deepEqual(imported.memory, document.memory);
      assert.deepEqual(imported.card.uid, uid);
      assert.deepEqual(imported.emulator, document.emulator);
      if (document.geometry.version.length) {
        const config = pages - 4;
        assert.equal(document.memory[config][3], 255);
        assert.deepEqual(document.memory[config + 2], [255, 255, 255, 255]);
        const count = Math.max(1, document.geometry.counters.length);
        assert.deepEqual(document.emulator.extra.slice(count, count + 2).flat(), document.geometry.version);
        assert(
          document.emulator.extra
            .slice(count + 2)
            .flat()
            .every((byte) => byte === 0)
        );
      }
    }
  }));

test('new NTAG and Ultralight C profiles expose writable user pages without authentication', () =>
  runtime(async () => {
    const uid = [4, 1, 2, 3, 4, 5, 6];
    const ntag = await blankType2(1100, uid);
    assert.deepEqual(ntag.memory.slice(4, 6).flat(), [1, 3, 0xa0, 0x0c, 0x34, 3, 0, 0xfe]);
    const ulc = await blankType2(1104, uid);
    assert.equal(ulc.memory[42][0], 0x30);
    assert.deepEqual(
      ulc.memory.slice(44).flat(),
      [0x49, 0x45, 0x4d, 0x4b, 0x41, 0x45, 0x52, 0x42, 0x21, 0x4e, 0x41, 0x43, 0x55, 0x4f, 0x59, 0x46]
    );
    for (const [model, document, lastUserPage] of [
      [8, ntag, 39],
      [19, ulc, 39]
    ]) {
      await withRuntime(async (call) => {
        const { writable } = await call({
          op: 'physical_permissions',
          model,
          memory: document.memory,
          key_b: [],
          authenticated: false
        });
        assert(writable.slice(4, lastUserPage + 1).every(Boolean));
        assert.equal(writable[0], false);
      });
    }
  }));

test('new HF documents reject unsupported identities before loading processing resources', async () => {
  await assert.rejects(blankClassic(8, [4, 1, 2, 3, 4, 5, 6]), /4-byte UID/);
  await assert.rejects(blankType2(1100, [1, 2, 3, 4]), /7-byte UID/);
  await assert.rejects(blankType2(1300, [4, 1, 2, 3, 4, 5, 6]), /supported Type 2/);
  await assert.rejects(blankType2(1100, [4, 1, 2, 3, 4, 5, 256]), /7-byte UID/);
});

test('draft restore preserves compatible SAK and unknown blocks but rejects mismatched geometry', () =>
  runtime(async () => {
    const document = await blankClassic(8, [1, 2, 3, 4]);
    document.card.sak = 0x28;
    document.memory[2] = null;

    const restored = await restoreDraft(JSON.stringify({ version: 1, document }));

    assert.equal(restored.card.sak, 0x28);
    assert.equal(restored.memory[2], null);
    assert.equal(restored.geometry.blocks, 64);
    await assert.rejects(exportClassic(restored), /every block/);

    document.memory.pop();

    await assert.rejects(restoreDraft(JSON.stringify({ version: 1, document })), /draft|geometry/i);
  }));

test('identity-only NFC-A drafts survive a browser reload', async () => {
  const document = {
    name: 'Tag-01020304',
    kind: 'identity',
    card: { uid: [1, 2, 3, 4], atqa: [0, 4], sak: 0x20, ats: [0x75] },
    memory: [],
    warnings: ['Memory reading is unsupported for this tag']
  };
  const restored = await restoreDraft(serializeDraft(document));

  assert.deepEqual(restored.card, document.card);
  assert.equal(restored.name, document.name);
  assert.equal(restored.kind, 'identity');
  assert.deepEqual(restored.memory, []);
  assert.deepEqual(restored.warnings, document.warnings);
});

test('portable drafts retain long imported filenames within the document size limit', () =>
  runtime(async () => {
    const blank = await blankClassic(8, [1, 2, 3, 4]);
    const name = 'long-import-'.repeat(17);
    const imported = await importNfc(await exportClassic(blank), name);
    const restored = await restoreDraft(serializeDraft(imported));

    assert.equal(restored.name, name);
    assert.deepEqual(restored.memory, imported.memory);
    assert.equal(await restoreDraft(' '.repeat(100001)), null);
  }));

test('MCT partial bytes survive import, export and drafts without enabling a complete image', () =>
  runtime(async () => {
    for (const sak of [9, 8, 16, 24]) {
      const document = await blankClassic(sak, [1, 2, 3, 4]);
      document.memory[2] = null;
      document.memory[3][0] = null;
      document.memory[3][5] = null;

      assert(!knownBlock(document.memory[3]));
      assert(memoryHex(document.memory[3]).startsWith('??FFFFFFFF??'));

      const text = await exportMct(document);

      assert(text.includes('--------------------------------'));

      const info = await inspectMct(text);

      assert.equal(info.sectors.length, document.geometry.sectors.length);

      const imported = await importMct(text, 'partial', sak, [1, 2, 3, 4]);

      assert.deepEqual(imported.memory, document.memory);
      assert.equal(imported.memory[3][6], 255);
      assert.throws(() => encodeSlotImage(imported), /complete/);
      await assert.rejects(exportClassic(imported), /every block/);

      const restored = await restoreDraft(JSON.stringify({ version: 1, document: imported }));

      assert.deepEqual(restored.memory, document.memory);
      assert.equal(await exportMct(restored), text);

      const edited = parseMemoryHex(memoryHex(restored.memory[3]).replace('FF0780', '7F0780'), 16);
      editBlock(restored, 3, edited);

      assert.equal(restored.memory[3][0], null);
      assert.equal(restored.memory[3][6], 127);
      assert(!knownBlock(restored.memory[3]));
      assert.throws(() => parseMemoryHex('F?'.repeat(16), 16), /hex bytes/);

      editBlock(restored, 2, Array(16).fill(0));
      editBlock(
        restored,
        3,
        document.memory[3].map((byte) => byte ?? 255)
      );

      assert(encodeSlotImage(restored).length > 64);
    }
  }));

test('MCT explicit geometry and long UID are required independently of memory bytes', () =>
  runtime(async () => {
    const text = '+Sector: 32\n' + ('FF'.repeat(16) + '\n').repeat(16);

    await assert.rejects(importMct(text, 'large', 8, [1, 2, 3, 4]), /selected card size/);
    await assert.rejects(importMct(text, 'large', 24, []), /4, 7 or 10/);

    for (const length of [4, 7, 10]) {
      const document = await importMct(text, 'large', 24, Array(length).fill(4));

      assert.equal(document.memory[0], null);
      assert.deepEqual(document.memory[128], Array(16).fill(255));
      assert.equal(document.card.atqa[1], { 4: 2, 7: 0x42, 10: 0x82 }[length]);
      assert.equal((await restoreDraft(JSON.stringify({ version: 1, document }))).card.uid.length, length);
    }
  }));

test('memory edits preserve long UID identity and reject malformed hex', () => {
  for (const length of [7, 10]) {
    const document = { card: { uid: Array(length).fill(4) }, memory: [Array(16).fill(0)] };
    editBlock(document, 0, Array(16).fill(1));

    assert.deepEqual(document.card.uid, Array(length).fill(4));
  }

  assert.throws(() => parseHex('FFZZ', 2), /hexadecimal/);
  assert.deepEqual(parseHex('aa bb', 2), [170, 187]);
});

test('slot decoder preserves UTF-8 names and rejects truncated or oversized names', () => {
  const name = new TextEncoder().encode('Entrée');
  const bytes = new Uint8Array([3, 233, 0, 100, 3, name.length, ...name, 0, 1, 2, 3, 4, 5, 6, 7, 8]);
  const result = decodeSlot(bytes);

  assert.equal(result.hf.revision, 0x01020304);
  assert.equal(result.lf.revision, 0x05060708);
  assert.equal(result.hf.name, 'Entrée');
  assert.equal(result.hf.type, 1001);
  assert(result.lf.enabled);
  assert.throws(() => decodeSlot(bytes.slice(0, -1)), /Invalid/);
  assert.throws(() => decodeSlot(new Uint8Array([0, 0, 0, 0, 0, 33, 0])), /Invalid/);
});

test('device requests time out while a response body is stalled', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });

  let bodyStarted = false;
  globalThis.fetch = async (_, { signal }) => ({
    ok: true,

    arrayBuffer() {
      bodyStarted = true;

      return new Promise((resolve, reject) =>
        signal.addEventListener('abort', () => reject(new Error('Aborted body')), { once: true })
      );
    }
  });

  try {
    const pending = request('/api/device');
    await Promise.resolve();
    await Promise.resolve();

    assert(bodyStarted);

    t.mock.timers.tick(20000);

    await assert.rejects(pending, /Aborted body/);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('RFID drafts retain their ID and conversion warning without a device session', async () => {
  const document = {
    name: 'EM410x/32-0102030405',
    type: 100,
    kind: 'lf',
    unitBytes: 5,
    card: { uid: [], atqa: [], sak: 0, ats: [] },
    memory: [[1, 2, 3, 4, 5]],
    warnings: ['Detected clock/32; Chameleon slot emulation uses clock/64']
  };
  const restored = await restoreDraft(JSON.stringify({ version: 1, document }));

  assert.deepEqual(restored.memory, document.memory);
  assert.deepEqual(restored.warnings, document.warnings);
  assert.equal(restored.slot, undefined);
});

const type2Card = { uid: [4, 1, 2, 3, 4, 5, 6], atqa: [0, 0x44], sak: 0, ats: [] };

test('Type 2 reads preserve partial data, discard wrapped pages and never claim masked credentials', () =>
  runtime(async () => {
    const device = {
      async command(command, [op, parameter, length, ...uid]) {
        if (command === 2026) {
          assert([0, 1].includes(op));

          return new Uint8Array();
        }

        assert.equal(command, 2028);
        assert.equal(length, 7);
        assert.deepEqual(uid.slice(0, 7), type2Card.uid);

        if (op === 0) {
          return new Uint8Array([0, 4, 4, 2, 1, 0, 0x0f, 3]);
        }
        if (op === 5) {
          const end = uid[7];

          assert(end >= parameter && end - parameter < 8 && end < 43);

          if (parameter <= 10 && end >= 10) {
            throw Object.assign(new Error('Protected'), { status: 2 });
          }

          return Uint8Array.from(
            Array.from({ length: end - parameter + 1 }, (_, i) => [parameter + i, 1, 2, 3]).flat()
          );
        }
        if (op === 1) {
          assert(parameter < 43);

          if (parameter === 10) {
            throw Object.assign(new Error('Protected'), { status: 2 });
          }

          return new Uint8Array([parameter, 1, 2, 3, ...Array(12).fill(0xee)]);
        }
        if (op === 3) {
          assert.equal(parameter, 2);

          return new Uint8Array([1, 2, 3]);
        }
        if (op === 2) {
          return new Uint8Array(32).fill(4);
        }

        assert.fail('Unexpected Type 2 operation');
      }
    };
    let updates = 0;
    const document = await readType2(device, type2Card, 'auto', () => updates++);

    assert(updates > 1);
    assert.deepEqual(document.memory[2], [2, 1, 2, 3]);
    assert.equal(document.memory[10], null);
    assert.equal(document.memory[43], null);
    assert.equal(document.memory[44], null);
    assert.deepEqual(document.emulator.extra[0], [1, 2, 3, 0]);
    assert(needsType2Defaults(document));

    const restored = await restoreDraft(JSON.stringify({ version: 1, document }));

    assert.deepEqual(restored.memory, document.memory);
    assert.deepEqual(restored.emulator, document.emulator);

    applyType2Defaults(restored);

    assert.deepEqual(restored.memory[43], [255, 255, 255, 255]);
    assert.deepEqual(restored.memory[44], [0, 0, 0, 0]);
    assert.equal(restored.memory[10], null);
    assert(!needsType2Defaults(restored));
    assert.throws(() => encodeSlotImage(restored), /complete/);

    editBlock(restored, 10, [1, 2, 3, 4]);

    assert(encodeSlotImage(restored).length > 100);
  }));

test('manual Ultralight reads need no invented version or hidden metadata', () =>
  runtime(async () => {
    const document = await readType2(
      {
        async command(command, [op, page]) {
          if (command === 2026) {
            return new Uint8Array();
          }

          assert.equal(op, 1);

          return new Uint8Array([page, 1, 2, 3, ...Array(12).fill(0)]);
        }
      },
      type2Card,
      'ultralight',
      () => {}
    );

    assert.equal(document.memory.length, 16);
    assert.deepEqual(document.emulator.extra, []);
    assert(!needsType2Defaults(document));
    assert(encodeSlotImage(document).length > 64);
  }));

test('NTAG fast ranges cover memory once and stop before credentials and memory end', () =>
  runtime(async () => {
    for (const [profile, pages, calls] of [
      ['ntag213', 45, 6],
      ['ntag215', 135, 17],
      ['ntag216', 231, 29]
    ]) {
      const ranges = [];
      const document = await readType2(
        {
          async command(command, [op, first, length, ...tail]) {
            if (command === 2026) {
              return new Uint8Array();
            }

            assert.equal(command, 2028);
            assert.equal(length, 7);

            if (op === 5) {
              const end = tail[7];
              ranges.push([first, end]);

              assert(end >= first && end - first < 8 && end < pages - 2);

              return Uint8Array.from(
                Array.from({ length: end - first + 1 }, (_, i) => [first + i, 1, 2, 3]).flat()
              );
            }
            if (op === 2) {
              return new Uint8Array(32);
            }
            if (op === 3) {
              return new Uint8Array(3);
            }

            assert.fail(`Unexpected operation ${op}`);
          }
        },
        type2Card,
        profile,
        () => {}
      );

      assert.equal(ranges.length, calls);

      for (let page = 0; page < pages - 2; page++) {
        assert.deepEqual(document.memory[page], [page, 1, 2, 3]);
      }

      assert.deepEqual(document.memory.slice(-2), [null, null]);
      assert.equal(ranges.at(-1)[1], pages - 3);
    }
  }));

test('a truncated FAST_READ response preserves earlier ranges and never assigns its bytes', () =>
  runtime(async () => {
    let lastDocument;

    await assert.rejects(
      readType2(
        {
          async command(command, [op, first, ...tail]) {
            if (command === 2026) {
              return new Uint8Array();
            }

            assert.equal(op, 5);

            const count = tail.at(-1) - first + 1;

            return new Uint8Array(count * 4 - (first === 8 ? 1 : 0)).fill(first);
          }
        },
        type2Card,
        'ntag213',
        (document) => {
          lastDocument = document;
        }
      ),
      /Invalid Type 2/
    );
    assert(lastDocument.memory.slice(0, 8).every(Boolean));
    assert(lastDocument.memory.slice(8).every((page) => page === null));
  }));

test('Stop during FAST_READ retains the returned range and sends no further read', () =>
  runtime(async () => {
    const controller = new AbortController();
    const device = new Device(controller.signal);
    const wasmFetch = globalThis.fetch;
    let ranges = 0;
    let ended = false;
    let lastDocument;
    globalThis.fetch = async (url, options) => {
      if (!url.startsWith('/api/')) {
        return wasmFetch(url, options);
      }
      if (url === '/api/session') {
        return new Response('{"session":1}', { headers: { 'Content-Type': 'application/json' } });
      }
      if (url.startsWith('/api/end')) {
        ended = true;

        return new Response();
      }

      const command = Number(url.match(/command\/(\d+)/)?.[1]);
      const data = Array.from(options.body);
      if (command === 2000) {
        return new Response(new Uint8Array([0, 0, 7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0, 0]));
      }
      if (command === 2026) {
        return new Response(new Uint8Array([0, 0]));
      }

      assert.equal(command, 2028);
      assert.equal(data[0], 5);

      ranges++;
      controller.abort();

      return new Response(new Uint8Array([0, 0, ...Array(32).fill(42)]));
    };

    try {
      const card = await device.begin();

      await assert.rejects(
        readType2(device, card, 'ntag213', (document) => {
          lastDocument = document;
        }),
        /cancelled/
      );
      assert.equal(ranges, 1);
      assert(lastDocument.memory.slice(0, 8).every((page) => page?.every((byte) => byte === 42)));
      assert(lastDocument.memory.slice(8).every((page) => page === null));
    } finally {
      await device.end();
    }

    assert(ended);
  }));

test('supported Type 2 Flipper files retain memory, RF counters, tearing and auth attempts', () =>
  runtime(async () => {
    for (const [profile, version, counters] of [
      ['ultralight', [], []],
      ['ultralight_c', [], []],
      ['ultralight_ev1_48', [0, 4, 3, 1, 1, 0, 11, 3], [0, 1, 2]],
      ['ultralight_ev1_128', [0, 4, 3, 1, 1, 0, 14, 3], [0, 1, 2]],
      ['ntag210', [0, 4, 4, 1, 1, 0, 11, 3], []],
      ['ntag212', [0, 4, 4, 1, 1, 0, 14, 3], []],
      ['ntag213', [0, 4, 4, 2, 1, 0, 15, 3], [2]],
      ['ntag215', [0, 4, 4, 2, 1, 0, 17, 3], [2]],
      ['ntag216', [0, 4, 4, 2, 1, 0, 19, 3], [2]]
    ]) {
      const document = await readType2(
        {
          async command(command, [op, page, ...rest]) {
            if (command === 2026) {
              return new Uint8Array();
            }
            if (op === 5) {
              return new Uint8Array((rest.at(-1) - page + 1) * 4).fill(page);
            }
            if (op === 1) {
              return new Uint8Array(16).fill(page);
            }
            if (op === 2) {
              return new Uint8Array(32).fill(42);
            }
            if (op === 3) {
              return new Uint8Array([0x12, 0x34, 0x56 + page]);
            }
            if (op === 4) {
              return new Uint8Array([page === 1 ? 0 : 0xbd]);
            }

            assert.fail('Unexpected operation');
          }
        },
        type2Card,
        profile,
        () => {}
      );
      applyType2Defaults(document);
      if (version.length) {
        document.emulator.extra[0][3] |= 7;
      }

      const text = await exportNfc(document);
      const imported = await importNfc(text, 'roundtrip');

      assert.deepEqual(imported.memory, document.memory);
      assert.deepEqual(imported.emulator, document.emulator);
      assert.deepEqual(imported.card.uid, type2Card.uid);

      for (const index of counters) {
        assert(text.includes(`Counter ${index}: ${0x12 + 0x34 * 256 + (0x56 + index) * 65536}`));
      }
      if (counters.length === 3) {
        assert(text.includes('Tearing 1: 00'));
      }
      if (version.length) {
        await assert.rejects(
          importNfc(
            text.replace('Failed authentication attempts: 7', 'Failed authentication attempts: 16'),
            'invalid'
          ),
          /emulator limit/
        );
      }
      if (profile.startsWith('ultralight_ev1')) {
        document.emulator.extra[counters.length][3] = 2;

        const variant = await importNfc(await exportNfc(document), 'capacitance variant');

        assert.deepEqual(variant.emulator.extra, document.emulator.extra);
      }
      document.memory[4] = null;

      await assert.rejects(exportNfc(document), /complete/);

      if (version.length) {
        document.emulator.extra[0] = null;
      }
      document.slot = { index: 3, sense: 2, revision: 123 };

      const draft = await restoreDraft(serializeDraft(document));

      assert.deepEqual(draft.memory, document.memory);
      assert.deepEqual(draft.emulator, document.emulator);
      assert.deepEqual(draft.card, document.card);
      assert.deepEqual(draft.warnings, document.warnings);
      assert.equal(draft.slot, undefined);
      assert.equal(draft.name, document.name);
      assert.throws(() => encodeSlotImage(draft), /complete/);
    }
  }));

test('Type 2 interruption flushes completed pages and refuses another UID or unsupported version', () =>
  runtime(async () => {
    let lastDocument;

    await assert.rejects(
      readType2(
        {
          async command(command, [op, page]) {
            if (command === 2026) {
              return new Uint8Array();
            }
            if (op === 1 && page === 2) {
              throw Object.assign(new Error('UID changed'), { status: 1 });
            }

            return new Uint8Array(16).fill(page);
          }
        },
        type2Card,
        'ultralight',
        (document) => {
          lastDocument = document;
        }
      ),
      /UID changed/
    );
    assert.deepEqual(lastDocument.memory[1], [1, 1, 1, 1]);
    assert.equal(lastDocument.memory[2], null);

    await assert.rejects(
      readType2(
        {
          async command(_, [op]) {
            assert.equal(op, 0);

            return new Uint8Array(8);
          }
        },
        type2Card,
        'auto',
        () => {}
      ),
      /Unsupported/
    );
  }));

test('aborted Type 2 campaigns release RF through uncancelled Device.end', () =>
  runtime(async () => {
    const controller = new AbortController();
    const device = new Device(controller.signal);
    const wasmFetch = globalThis.fetch;
    let powered = false;
    let ended = false;
    globalThis.fetch = async (url, options) => {
      if (!url.startsWith('/api/')) {
        return wasmFetch(url, options);
      }

      const payload = Array.from(options?.body || []);
      if (url === '/api/session') {
        return new Response(JSON.stringify({ session: 1 }), {
          headers: { 'Content-Type': 'application/json' }
        });
      }
      if (url.startsWith('/api/end')) {
        powered = false;
        ended = true;

        return new Response(new Uint8Array());
      }

      const command = Number(url.match(/command\/(\d+)/)?.[1]);
      let data = [];
      if (command === 2000) {
        data = [7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0, 0];
      } else if (command === 2026) {
        powered = !!payload[0];
      } else if (command === 2028) {
        assert(powered);

        data = Array(16).fill(4);
        controller.abort();
      } else {
        assert.fail(`Unexpected command ${command}`);
      }

      return new Response(new Uint8Array([0, 0, ...data]));
    };

    try {
      const card = await device.begin();

      await assert.rejects(
        readType2(device, card, 'ultralight', () => {}),
        /cancelled/
      );
      assert(powered);
    } finally {
      await device.end();
    }

    assert(ended && !powered);
  }));

test('ST25TB shared .nfc conversion preserves system data and requires explicit slot Chip ID', () =>
  runtime(async () => {
    const document = hfDocument({
      protocol: 'st25tb',
      uid: [0xd0, 2, 0x33, 5, 4, 3, 2, 1],
      chip_id: 0x5a,
      blocks: 16,
      block_size: 4,
      system: [255, 165, 18, 52]
    });
    document.memory = Array.from({ length: 16 }, (_, i) => [i * 4, i * 4 + 1, i * 4 + 2, i * 4 + 3]);

    const text = await exportNfc(document);

    assert.match(text, /System OTP Block: FF A5 12 34/);

    const imported = await importNfc(text, 'ST25TB test');

    assert.deepEqual(imported.memory, document.memory);
    assert.deepEqual(imported.card.uid, document.card.uid);
    assert.equal(imported.card.chip_id, null);
    assert(needsHfDefaults(imported));
    assert.throws(() => encodeSlotImage(imported), /ST25TB save/);

    applyHfDefaults(imported, 0x42);

    const image = encodeSlotImage(imported);

    assert.equal(image[81], 0x42);
    assert.deepEqual((await restoreDraft(serializeDraft(imported))).card.system, [255, 165, 18, 52]);
  }));

test('NFC-V partial draft preserves unknowns and explicit export defaults do not change capture', () =>
  runtime(async () => {
    const document = hfDocument({
      protocol: 'nfc_v',
      uid: [224, 4, 1, 1, 2, 3, 4, 5],
      blocks: 28,
      block_size: 4,
      dsfid: 12,
      afi: null,
      ic_ref: null
    });
    document.memory[0] = [1, 2, 3, 4];

    const restored = await restoreDraft(serializeDraft(document));

    assert.equal(restored.memory[1], null);
    assert.equal(restored.locks[0], null);
    await assert.rejects(exportNfc(restored), /all user blocks/);

    restored.memory = Array.from({ length: 28 }, () => [1, 2, 3, 4]);

    await assert.rejects(exportNfc(restored), /Explicit emulator defaults/);

    applyHfDefaults(restored);

    const text = await exportNfc(restored);
    const imported = await importNfc(text, 'SLIX test');

    assert.equal(imported.memory.length, 28);
    assert.equal(imported.card.dsfid, 12);
    assert.equal(restored.locks[0], null);
    assert.equal((await restoreDraft(serializeDraft(restored))).emulator.model, 3);
    assert.deepEqual(imported.memory, restored.memory);
  }));

test('partial recovery retains validated keys independently of unread memory and accepts old drafts', () =>
  runtime(async () => {
    const document = await blankClassic(8, [1, 2, 3, 4]);
    document.memory.fill(null);
    document.recoveredKeys = Array.from({ length: 16 }, () => ({ key_a: null, key_b: null }));
    document.recoveredKeys[2].key_a = [1, 2, 3, 4, 5, 6];

    const restored = await restoreDraft(serializeDraft(document));

    assert.deepEqual(restored.recoveredKeys, document.recoveredKeys);
    assert(restored.memory.every((block) => block === null));

    for (const invalid of [
      [],
      Array(16).fill({ key_a: [1], key_b: null }),
      Array(16).fill({ key_a: null, key_b: [-1, 0, 0, 0, 0, 0] })
    ]) {
      await assert.rejects(
        restoreDraft(serializeDraft({ ...document, recoveredKeys: invalid })),
        /Invalid recovered keys/
      );
    }

    delete document.recoveredKeys;

    assert.equal((await restoreDraft(serializeDraft(document))).recoveredKeys, undefined);
  }));

async function readClassicDocument(sak = 8) {
  const document = await blankClassic(sak, [1, 2, 3, 4]);
  document.recoveredKeys = document.geometry.sectors.map(() => ({
    key_a: Array(6).fill(255),
    key_b: Array(6).fill(255),
    origin_b: 'Trailer'
  }));
  document.classicRead = classicReadSnapshot({
    ...document,
    summary: { mapping: document.recoveredKeys }
  });
  return document;
}

test('Classic changes use a session-only snapshot, skip block 0 and track undo and unknown bytes', () =>
  runtime(async () => {
    const document = await readClassicDocument(24);
    const reading = document.classicRead;
    assert.deepEqual(classicChanges(document), { writes: [], unknown: [] });
    editBlock(document, 128, Array(16).fill(42));
    assert.deepEqual(
      classicChanges(document).writes.map((unit) => unit.index),
      [128]
    );
    assert.deepEqual(reading.memory[128], Array(16).fill(0));
    editBlock(document, 128, reading.memory[128]);
    editBlock(document, 143, Array(16).fill(0));
    editBlock(document, 0, Array(16).fill(0));
    reading.memory[129] = null;
    editBlock(document, 129, Array(16).fill(42));
    editBlock(document, 130, Array(16).fill(null));
    assert.deepEqual(classicChanges(document).unknown, [129, 130]);
    assert.deepEqual(
      classicChanges(document).writes.map((unit) => unit.index),
      [143]
    );
    assert.deepEqual(reading.card.uid, [1, 2, 3, 4]);
    assert.equal(JSON.parse(serializeDraft(document)).document.classicRead, undefined);
    const restored = await restoreDraft(serializeDraft(document));
    assert.equal(classicChanges(restored), null);
  }));

for (const keyB of [false, true]) {
  test(`Classic changes write only edited known blocks using original key ${keyB ? 'B' : 'A'}`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument();
      const reading = document.classicRead;
      // These valid access bytes require B for data writes; B is not readable.
      if (keyB) {
        reading.memory[3].splice(6, 3, 0x0f, 0x00, 0xff);
        document.memory[3] = [...reading.memory[3]];
        reading.keys[0].origin_b = 'Dictionary';
        reading.keys[0].key_b = [1, 2, 3, 4, 5, 6];
      }
      document.memory[8] = null;
      reading.memory[8] = null;
      editBlock(document, 1, Array(16).fill(42));
      editBlock(document, 2, Array(16).fill(43));
      editBlock(document, 0, Array(16).fill(0));
      document.recoveredKeys[0].key_a.fill(0);
      const writes = [];
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async () => reading.card);
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'command', async () => assert.fail('No key search or memory reread'));
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        assert.equal(command, 2030);
        assert.equal(payload.length, 59);
        assert.deepEqual(payload.slice(0, 9), [1, 4, 1, 2, 3, 4, 0, 0, 0]);
        assert.equal(payload[10], keyB ? 2 : 1);
        assert.deepEqual(payload.slice(11, 17), keyB ? [1, 2, 3, 4, 5, 6] : Array(6).fill(255));
        assert.deepEqual(payload.slice(27, 43), Array(16).fill(0));
        writes.push(payload[9]);
        return Uint8Array.from(payload.slice(43));
      });
      const progress = [];
      await writeClassicChanges(document, undefined, (message) => progress.push(message), {
        confirmChanges: async ({ uid, writes }) => {
          assert.deepEqual(uid, [1, 2, 3, 4]);
          assert.deepEqual(
            writes.map((unit) => unit.index),
            [1, 2]
          );
          return true;
        }
      });
      assert.deepEqual(writes, [1, 2]);
      assert.equal(classicChanges(document).writes.length, 0);
      assert.deepEqual(reading.memory[1], Array(16).fill(42));
      assert.equal(progress.at(-1), '2 blocks written and verified');
      assert(ended);
    }));
}

for (const scenario of ['cancel review', 'missing key', 'unread trailer', 'unknown bytes']) {
  test(`Classic changes perform no RF operations: ${scenario}`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument();
      editBlock(document, 1, Array(16).fill(42));
      if (scenario === 'missing key') {
        document.classicRead.keys[0].key_a = null;
      } else if (scenario === 'unread trailer') {
        document.classicRead.memory[3] = null;
      } else if (scenario === 'unknown bytes') {
        document.classicRead.memory[1] = null;
      }
      t.mock.method(Device.prototype, 'begin', async () => assert.fail('No RF reservation'));
      t.mock.method(Device.prototype, 'writeCommand', async () => assert.fail('No write'));
      const work = writeClassicChanges(document, undefined, () => {}, {
        confirmChanges: async () => {
          assert.equal(scenario, 'cancel review');
          return false;
        }
      });
      if (scenario === 'cancel review') {
        await work;
      } else {
        await assert.rejects(work, /No known write key|complete original/);
      }
      assert(document.classicRead);
    }));
}

for (const scenario of ['changed UID', 'lost reply', 'mismatch', 'stop', 'cleanup failed']) {
  test(`Classic changes retain confirmed writes, invalidate uncertain state and release ownership: ${scenario}`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument();
      const reading = document.classicRead;
      editBlock(document, 1, Array(16).fill(42));
      editBlock(document, 2, Array(16).fill(43));
      const controller = new AbortController();
      const writes = [];
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async () => ({
        ...reading.card,
        uid: scenario === 'changed UID' ? [4, 3, 2, 1] : reading.card.uid
      }));
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
        if (scenario === 'cleanup failed') {
          throw new Error('Cleanup failed');
        }
      });
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        writes.push(payload[9]);
        if (payload[9] === 2 && scenario === 'lost reply') {
          throw new Error('Write outcome unknown');
        }
        if (payload[9] === 2 && scenario === 'mismatch') {
          return new Uint8Array(16);
        }
        if (scenario === 'stop') {
          controller.abort();
        }
        return Uint8Array.from(payload.slice(43));
      });
      const confirmed = { 'changed UID': 0, 'lost reply': 1, mismatch: 1, stop: 1, 'cleanup failed': 2 }[
        scenario
      ];
      await assert.rejects(
        writeClassicChanges(document, controller.signal, () => {}, { confirmChanges: async () => true }),
        (error) =>
          error.message.startsWith(`${confirmed}/2 blocks written and verified.`) &&
          /Read the tag again/.test(error.message)
      );
      let expectedWrites = [1, 2];
      if (scenario === 'changed UID') {
        expectedWrites = [];
      } else if (scenario === 'stop') {
        expectedWrites = [1];
      }
      assert.deepEqual(writes, expectedWrites);
      assert.equal(ended, true);
      assert.equal(classicChanges(document), null);
      assert.deepEqual(reading.memory[1], Array(16).fill(confirmed ? 42 : 0));
      assert.deepEqual(document.memory[2], Array(16).fill(43));
    }));
}

for (const scenario of [
  'upper 2K data',
  '2K clone',
  'cancel 2K',
  '2K source on 1K',
  '1K source on 2K',
  '2K becomes 1K during review',
  '1K becomes 2K during review',
  '2K ATS changes during review',
  '1K unknown ATS',
  'legacy inspection',
  'unsupported inspection',
  'invalid ATS',
  'lost upper write reply'
]) {
  test(`Classic copy establishes fresh target capacity: ${scenario}`, (t) =>
    runtime(async () => {
      const oneKSource = scenario.startsWith('1K');
      const document = await readClassicDocument(oneKSource ? 8 : 16);
      const targetMemory = structuredClone(document.memory);
      const card = { ...document.card, sak: 8, ats: [] };
      const ats = [12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x2f, 0x2f, 1, 0xbc, 0xd6, 0x60, 0xd3];
      // Neither a saved source ATS nor its model establishes the target's size.
      document.card.ats = ats.slice(0, -2);
      document.classicObservation = { card: structuredClone(card), type: 1002 };
      editBlock(document, 1, Array(16).fill(42));
      if (!oneKSource) {
        editBlock(document, 126, Array(16).fill(43));
      }
      let targetTwoK = !scenario.startsWith('2K source on') && scenario !== '1K becomes 2K during review';
      let inspections = 0;
      let changedAts = false;
      let reviewed = false;
      let ended = false;
      const writes = [];
      const shouldReachReview = [
        'upper 2K data',
        '2K clone',
        'cancel 2K',
        '2K becomes 1K during review',
        '1K becomes 2K during review',
        '2K ATS changes during review',
        'lost upper write reply'
      ].includes(scenario);
      t.mock.method(Device.prototype, 'begin', async () => card);
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        if (scenario === '2K clone' && command === 2000) {
          return Uint8Array.from([4, ...card.uid, 4, 0, 8, 0]);
        }
        assert.equal(command, 2040);
        assert.equal(payload.length, 16 + payload[14] * 6);
        assert.equal(payload.at(-1), 1);
        inspections++;
        if (scenario === 'unsupported inspection') {
          throw Object.assign(new Error('Unsupported inspection'), { status: 0x67 });
        }
        const legacy = [0, 0, 0, 1, 0, ...Array(6).fill(0)];
        if (scenario === 'legacy inspection') {
          return Uint8Array.from(legacy);
        }
        let frame = targetTwoK ? [...ats] : [];
        if (scenario === '1K unknown ATS') {
          frame = [2, 0, 16, 45];
        } else if (changedAts) {
          frame = [12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x2f, 0x2f, 0, 0x35, 0xc7, 0x60, 0xd3];
        }
        if (scenario === 'invalid ATS') {
          frame[13] ^= 1;
        }
        return Uint8Array.from([...legacy, 1, frame.length, ...frame]);
      });
      t.mock.method(Device.prototype, 'findKey', async () => {
        assert(shouldReachReview, 'Capacity must be checked before reading target memory');
        return Array(6).fill(255);
      });
      t.mock.method(Device.prototype, 'readBlock', async (block) => [...targetMemory[block]]);
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        assert.equal(command, 2030);
        assert.equal(payload[0], 6);
        assert.equal(inspections, 2);
        writes.push(payload[9]);
        if (scenario === 'lost upper write reply' && payload[9] === 126) {
          throw new Error('Write outcome unknown');
        }
        targetMemory[payload[9]] = payload.slice(43);
        return Uint8Array.from(payload.slice(43));
      });
      const work = copyToTag(document, undefined, () => {}, {
        requestCredential: async () => assert.fail('Classic copy uses recovered keys'),
        confirmCopy: async (plan) => {
          assert(shouldReachReview);
          reviewed = true;
          assert.equal(plan.writes, oneKSource ? 1 : 2);
          if (scenario === '2K ATS changes during review') {
            changedAts = true;
          } else if (scenario.includes('during review')) {
            targetTwoK = !targetTwoK;
          }
          if (scenario === 'cancel 2K') {
            return null;
          }
          return scenario === '2K clone' ? 'clone' : 'data';
        }
      });
      if (['upper 2K data', 'cancel 2K', '2K clone'].includes(scenario)) {
        await work;
      } else {
        const reason =
          {
            'lost upper write reply': /1\/2.*outcome unknown/,
            'legacy inspection': /firmware.*capacity/i,
            'unsupported inspection': /firmware.*capacity/i,
            'invalid ATS': /ATS/,
            '1K unknown ATS': /ATS/,
            '2K ATS changes during review': /target changed during review/
          }[scenario] || /target Classic size differs/;
        await assert.rejects(work, reason);
      }
      assert.equal(reviewed, shouldReachReview);
      assert.deepEqual(
        writes,
        ['upper 2K data', '2K clone', 'lost upper write reply'].includes(scenario) ? [1, 126] : []
      );
      let expectedInspections = reviewed && scenario !== 'cancel 2K' ? 2 : 1;
      if (scenario === '2K clone') {
        expectedInspections++;
      }
      assert.equal(inspections, expectedInspections);
      assert(ended);
    }));
}

for (const sak of [9, 16, 24]) {
  test(`Classic copy keeps the unambiguous capacity path (SAK ${sak})`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument(sak);
      document.card.sak = sak;
      document.memory[0][5] = sak;
      const target = structuredClone(document.memory);
      const lastData = document.geometry.blocks - 2;
      editBlock(document, lastData, Array(16).fill(42));
      let ended = false;
      const writes = [];
      t.mock.method(Device.prototype, 'begin', async () => structuredClone(document.card));
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'command', async () => assert.fail('No additional capability probes'));
      t.mock.method(Device.prototype, 'findKey', async () => Array(6).fill(255));
      t.mock.method(Device.prototype, 'readBlock', async (block) => [...target[block]]);
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        assert.equal(command, 2030);
        assert.equal(payload[0], { 9: 5, 16: 6, 24: 7 }[sak]);
        writes.push(payload[9]);
        return Uint8Array.from(payload.slice(43));
      });
      await copyToTag(document, undefined, () => {}, {
        requestCredential: async () => assert.fail('No Type 2 credential'),
        confirmCopy: async ({ writes }) => {
          assert.equal(writes, 1);
          return 'data';
        }
      });
      assert.deepEqual(writes, [lastData]);
      assert(ended);
    }));
}

for (const scenario of ['cancel', 'same tag', 'other tag', 'already matches']) {
  test(`Classic copy invalidates the read reference only when writing the original tag (${scenario})`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument();
      const reading = document.classicRead;
      const card = structuredClone(reading.card);
      if (scenario === 'other tag') {
        card.uid = [4, 3, 2, 1];
      }
      if (scenario !== 'already matches') {
        editBlock(document, 1, Array(16).fill(42));
      }
      const targetMemory = structuredClone(reading.memory);
      targetMemory[0].splice(0, 4, ...card.uid);
      let writes = 0;
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async () => card);
      t.mock.method(Device.prototype, 'findKey', async () => Array(6).fill(255));
      t.mock.method(Device.prototype, 'readBlock', async (block) => [...targetMemory[block]]);
      t.mock.method(Device.prototype, 'command', async (command) => {
        if (command === 2032) {
          return Uint8Array.from([0, 0, 2]);
        }
        assert.equal(command, 2040);
        return Uint8Array.from([0, 0, 0, 1, 0, ...Array(6).fill(0), 1, 0]);
      });
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        assert.equal(command, 2030);
        assert.equal(payload[9], 1);
        writes++;
        return Uint8Array.from(payload.slice(43));
      });
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      await copyToTag(document, undefined, () => {}, {
        requestCredential: async () => assert.fail('Classic copy uses recovered keys'),
        confirmCopy: async (plan) => {
          assert.equal(plan.writes, Number(scenario !== 'already matches'));
          return scenario === 'same tag' || scenario === 'other tag' ? 'data' : null;
        }
      });
      assert.equal(writes, Number(scenario === 'same tag' || scenario === 'other tag'));
      assert.equal(document.classicRead, scenario === 'same tag' ? undefined : reading);
      assert(ended);
    }));
}

for (const scenario of [
  'keys',
  'key B',
  'large sector',
  'hidden original keys',
  'denied',
  'command rejected',
  'lost trailer reply',
  'stop'
]) {
  test(`Classic trailer changes preflight all security writes and write data first: ${scenario}`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument(scenario === 'large sector' ? 24 : 8);
      const reading = document.classicRead;
      const sector = scenario === 'large sector' ? 39 : 0;
      const group = document.geometry.sectors[sector];
      const after = [...document.memory[group.trailer]];
      after.splice(0, 6, 1, 2, 3, 4, 5, 6);
      after.splice(10, 6, 6, 5, 4, 3, 2, 1);
      if (['key B', 'command rejected'].includes(scenario)) {
        reading.keys[sector].origin_b = 'Dictionary';
        reading.memory[group.trailer].splice(6, 3, 0x7f, 0x07, 0x88);
      }
      if (scenario === 'hidden original keys') {
        reading.memory[group.trailer].splice(0, 6, ...Array(6).fill(null));
      }
      editBlock(document, group.first + 1, Array(16).fill(42));
      editBlock(document, group.trailer, after);
      const controller = new AbortController();
      const events = [];
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async () => reading.card);
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        assert.equal(command, 2034);
        assert.equal(payload.length, 38);
        assert.equal(payload[37], 1);
        assert.equal(payload[9], sector);
        assert.deepEqual(payload.slice(17, 21), reading.memory[group.trailer].slice(6, 10));
        events.push(`check ${payload[10]}`);
        if (scenario === 'command rejected' || (scenario === 'key B' && payload[10] === 1)) {
          // trailer_set returns CMD_ERR for denied permissions or stale access bytes.
          throw Object.assign(new Error('Command rejected'), { status: 0x75 });
        }
        if (scenario === 'denied') {
          throw Object.assign(new Error('Permissions denied'), { status: 0x70 });
        }
        return new Uint8Array();
      });
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        events.push(`write ${command}`);
        if (command === 2030) {
          if (scenario === 'stop') {
            controller.abort();
          }
          return Uint8Array.from(payload.slice(43));
        }
        assert.equal(command, 2034);
        assert.equal(payload.length, 37);
        assert.equal(payload[10], scenario === 'key B' ? 2 : 1);
        assert.deepEqual(payload.slice(21, 27), after.slice(0, 6));
        assert.deepEqual(payload.slice(27, 33), after.slice(10));
        assert.deepEqual(payload.slice(33), after.slice(6, 10));
        if (scenario === 'lost trailer reply') {
          throw new Error('Write outcome unknown');
        }
        return new Uint8Array();
      });
      const progress = [];
      const work = writeClassicChanges(document, controller.signal, (message) => progress.push(message), {
        confirmChanges: async ({ data, trailers }) => {
          assert.equal(data.length, 1);
          assert.equal(trailers.length, 1);
          assert.equal(trailers[0].sector, sector);
          return true;
        }
      });
      if (['denied', 'command rejected', 'lost trailer reply', 'stop'].includes(scenario)) {
        await assert.rejects(
          work,
          ['denied', 'command rejected'].includes(scenario) ? /0\/2.*Sector/ : /1\/2.*Read the tag again/
        );
      } else {
        await work;
        assert.match(progress.at(-1), /2 blocks written and verified.*Read the tag again/);
        assert.deepEqual(reading.memory[group.trailer], after);
      }
      const expected = ['key B', 'command rejected'].includes(scenario)
        ? ['check 1', 'check 2']
        : ['check 1'];
      if (!['denied', 'command rejected'].includes(scenario)) {
        expected.push('write 2030');
        if (scenario !== 'stop') {
          expected.push('write 2034');
        }
      }
      assert.deepEqual(events, expected);
      assert.equal(document.classicRead, undefined);
      assert.equal(document.recoveredKeys, undefined);
      assert(ended);
    }));
}

for (const scenario of ['copy', 'changed keys', 'tag removed', 'bad capture']) {
  test(`Classic copy rechecks learned target keys without repeating dictionary searches (${scenario})`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument();
      const { nonce_keys: dictionary } = await withRuntime((call) =>
        call({ op: 'dictionary', text: '', anchor: null, profile: 'Extended' })
      );
      const targetKeys = document.geometry.sectors.map(() => [dictionary[150], dictionary[500]]);
      const targetMemory = structuredClone(document.memory);
      for (const group of document.geometry.sectors) {
        targetMemory[group.trailer] = [...Array(6).fill(0), 0x7f, 0x07, 0x88, 0x69, ...Array(6).fill(0)];
      }
      const card = { ...document.card, uid: [4, 3, 2, 1] };
      targetMemory[0].splice(0, 4, ...card.uid);
      editBlock(document, 1, Array(16).fill(42));
      const checks = document.geometry.sectors.map(() => [0, 0]);
      const progress = [];
      let reviewedChecks;
      let writes = 0;
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async () => card);
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        if (command === 2040) {
          return Uint8Array.from([0, 0, 0, 1, 0, ...Array(6).fill(0), 1, 0]);
        }
        if (command === 2026) {
          return new Uint8Array();
        }
        assert.equal(command, 2015);
        const sector = payload[0] / 4;
        const keyB = Number(payload[1] === 0x61);
        checks[sector][keyB]++;
        const key = targetKeys[sector][keyB];
        for (let offset = 3; offset < payload.length; offset += 6) {
          if (payload.slice(offset, offset + 6).join() === key.join()) {
            return Uint8Array.from([1, ...key]);
          }
        }
        throw Object.assign(new Error('Authentication rejected'), { status: 6 });
      });
      t.mock.method(Device.prototype, 'readBlock', async (block, key, keyB) => {
        assert.deepEqual(key, targetKeys[Math.floor(block / 4)][Number(keyB)]);
        return [...targetMemory[block]];
      });
      t.mock.method(Device.prototype, 'probeMagic', async () => ({ flags: 0, gen2: 2 }));
      t.mock.method(Device.prototype, 'collectNonces', async () => {
        if (scenario === 'tag removed') {
          throw Object.assign(new Error('Card not found'), { status: 1 });
        }
        if (scenario === 'bad capture') {
          throw new Error('Invalid capture length');
        }
        throw Object.assign(new Error('Capture unsupported'), { status: 0x69 });
      });
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        assert.equal(command, 2030);
        assert.equal(payload[9], 1);
        writes++;
        return Uint8Array.from(payload.slice(43));
      });
      const work = copyToTag(document, undefined, (message) => progress.push(message), {
        requestCredential: async () => assert.fail('No Type 2 credential'),
        confirmCopy: async ({ clone, writes }) => {
          assert.match(clone.reason, /Block 0 requires a supported Gen1A or CUID target/);
          assert.equal(writes, 1);
          reviewedChecks = structuredClone(checks);
          assert(reviewedChecks[0][0] > 1);
          assert(reviewedChecks[0][1] > 1);
          assert.deepEqual(
            reviewedChecks.slice(1),
            Array.from({ length: 15 }, () => [1, 1])
          );
          if (scenario === 'changed keys') {
            targetKeys[2] = [dictionary[160], dictionary[510]];
          }
          return 'data';
        }
      });
      if (scenario === 'changed keys') {
        await assert.rejects(work, /target changed during review/);
        assert.equal(writes, 0);
      } else if (scenario === 'tag removed' || scenario === 'bad capture') {
        await assert.rejects(work, /Card not found|Invalid capture length/);
        assert.equal(writes, 0);
        assert.equal(reviewedChecks, undefined);
      } else {
        await work;
        assert.equal(writes, 1);
        assert.deepEqual(
          checks,
          reviewedChecks.map(([a, b]) => [a + 1, b + 1])
        );
      }
      assert(
        progress.some((message) => /sector 1 \/ 16 · key B · [1-9]\d* \/ \d+ keys tested/.test(message))
      );
      assert(ended);
    }));
}

for (const scenario of [
  'Gen1A',
  'CUID',
  'data only',
  'cancel',
  'Gen4',
  'FUID',
  'partial source',
  'invalid block 0',
  'trailer denied',
  'changed target',
  'lost reply',
  'radio mismatch',
  'ATQA mismatch',
  'ATS mismatch',
  'lost trailer reply',
  'cleanup failed',
  'unsupported selected',
  'activation byte 5',
  'activation byte 6',
  'activation byte 7',
  'data with activation differences'
]) {
  test(`Classic clone checks identity and security and never silently falls back: ${scenario}`, (t) =>
    runtime(async () => {
      const document = await readClassicDocument();
      const targetMemory = structuredClone(document.memory);
      const targetCard = { ...document.card, uid: [4, 3, 2, 1] };
      targetMemory[0].splice(0, 4, ...targetCard.uid);
      editBlock(document, 1, Array(16).fill(42));
      document.memory[0][15] = 99;
      document.memory[3].splice(0, 6, 1, 2, 3, 4, 5, 6);
      document.memory[3].splice(10, 6, 6, 5, 4, 3, 2, 1);
      if (scenario === 'partial source') {
        document.memory[5] = null;
      }
      if (scenario === 'invalid block 0') {
        document.memory[0][4] ^= 1;
      }
      if (scenario.startsWith('activation byte')) {
        document.memory[0][Number(scenario.at(-1))] ^= 1;
      }
      if (scenario === 'data with activation differences') {
        document.memory[0][5] ^= 1;
      }
      const writes = [];
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async () => targetCard);
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
        if (scenario === 'cleanup failed') {
          throw new Error('Cleanup failed');
        }
      });
      t.mock.method(Device.prototype, 'findKey', async () => Array(6).fill(255));
      t.mock.method(Device.prototype, 'readBlock', async (index) => [...targetMemory[index]]);
      t.mock.method(Device.prototype, 'probeMagic', async (card, key) => ({
        flags: { Gen4: 256, FUID: 9, CUID: key ? 4 : 0, 'unsupported selected': 256 }[scenario] ?? 1,
        gen2: scenario === 'CUID' && key ? 3 : 0
      }));
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        if (command === 2040) {
          return Uint8Array.from([0, 0, 0, 1, 0, ...Array(6).fill(0), 1, 0]);
        }
        if (command === 2038) {
          assert.equal(payload.length, 60);
          assert.equal(payload[10], scenario === 'CUID' ? 1 : 0);
          return Uint8Array.from(targetMemory[0]);
        }
        if (command === 2034) {
          assert.equal(payload.length, 38);
          if (scenario === 'trailer denied') {
            throw Object.assign(new Error('Permissions denied'), { status: 0x70 });
          }
          return new Uint8Array();
        }
        assert.equal(command, 2000);
        return Uint8Array.from([
          4,
          ...document.card.uid,
          ...(scenario === 'ATQA mismatch' ? [5, 0] : [...document.card.atqa].reverse()),
          scenario === 'radio mismatch' ? 9 : document.card.sak,
          ...(scenario === 'ATS mismatch' ? [1, 0] : [0])
        ]);
      });
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        writes.push(command);
        if (command === 2030) {
          targetMemory[payload[9]] = [...payload.slice(43)];
          return Uint8Array.from(payload.slice(43));
        }
        if (command === 2038) {
          if (scenario === 'lost reply') {
            throw new Error('Write outcome unknown');
          }
          assert.deepEqual(payload.slice(43), document.memory[0]);
          targetMemory[0] = [...payload.slice(43)];
          return Uint8Array.from(payload.slice(43));
        }
        assert.equal(command, 2034);
        assert.deepEqual(payload.slice(1, 5), document.card.uid);
        if (scenario === 'lost trailer reply') {
          throw new Error('Write outcome unknown');
        }
        return new Uint8Array();
      });
      const unavailable = [
        'Gen4',
        'FUID',
        'partial source',
        'invalid block 0',
        'trailer denied',
        'unsupported selected'
      ].includes(scenario);
      const progress = [];
      const verified = [];
      const work = copyToTag(
        document,
        undefined,
        (message, detail) => {
          progress.push(message);
          if (detail?.total) {
            verified.push(detail);
          }
        },
        {
          requestCredential: async () => assert.fail('No Type 2 credential'),
          confirmCopy: async ({ clone }) => {
            assert.equal(!!clone.reason, unavailable);
            if (scenario === 'unsupported selected') {
              return 'clone';
            }
            if (scenario === 'data with activation differences') {
              return 'data';
            }
            if (scenario === 'cancel' || unavailable) {
              return null;
            }
            if (scenario === 'changed target') {
              targetMemory[2][0] = 55;
            }
            return scenario === 'data only' ? 'data' : 'clone';
          }
        }
      );
      if (
        [
          'changed target',
          'lost reply',
          'radio mismatch',
          'ATQA mismatch',
          'ATS mismatch',
          'lost trailer reply',
          'cleanup failed',
          'unsupported selected'
        ].includes(scenario)
      ) {
        await assert.rejects(
          work,
          /changed during review|1\/3.*unknown|3\/3.*radio identity|2\/3.*unknown|3\/3.*Cleanup failed|mode is unavailable/
        );
      } else {
        await work;
      }
      let expected = [];
      if (['Gen1A', 'CUID'].includes(scenario) || scenario.startsWith('activation byte')) {
        expected = [2030, 2038, 2034];
        assert.match(progress.at(-1), /^Clone verified/);
        assert.deepEqual(
          verified,
          [0, 1, 2, 3].map((completed) => ({
            completed,
            total: 3,
            unitLabel: 'block'
          }))
        );
      } else if (scenario === 'data only' || scenario === 'data with activation differences') {
        expected = [2030];
      } else if (scenario === 'lost reply') {
        expected = [2030, 2038];
      } else if (
        ['radio mismatch', 'ATQA mismatch', 'ATS mismatch', 'lost trailer reply', 'cleanup failed'].includes(
          scenario
        )
      ) {
        expected = [2030, 2038, 2034];
      }
      assert.deepEqual(writes, expected);
      if (scenario === 'lost reply') {
        assert.equal(verified.at(-1).completed, 1);
      }
      if (scenario === 'lost trailer reply') {
        assert.equal(verified.at(-1).completed, 2);
      }
      assert(ended);
    }));
}

for (const scenario of ['write', 'stale page', 'different tag', 'cancel', 'lost reply', 'locked']) {
  test(`Type 2 changes bind the session read and preserve protected pages (${scenario})`, (t) =>
    runtime(async () => {
      const doc = await blankType2(1100, type2Card.uid);
      doc.type2Read = {
        type: doc.type,
        card: structuredClone(doc.card),
        memory: structuredClone(doc.memory)
      };
      const target = structuredClone(doc.memory);
      doc.memory[6] = [42, 0, 0, 0];
      doc.memory[0][0] = 99;
      if (scenario === 'stale page') {
        target[6][0] = 12;
      }
      if (scenario === 'locked') {
        target[2][2] = 0xff;
        target[2][3] = 0xff;
      }
      let ended = false;
      let writes = 0;
      t.mock.method(Device.prototype, 'begin', async function () {
        this.card = structuredClone(type2Card);
        if (scenario === 'different tag') {
          this.card.uid[1] ^= 1;
        }
        return this.card;
      });
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        assert.equal(command, 2028);
        if (payload[0] === 0) {
          return Uint8Array.from([0, 4, 4, 2, 1, 0, 0x0f, 3]);
        }
        if (payload[0] === 5) {
          return Uint8Array.from(target.slice(payload[1], payload.at(-1) + 1).flat());
        }
        return Uint8Array.from([...target[payload[1]], ...Array(12).fill(255)]);
      });
      t.mock.method(Device.prototype, 'writeCommand', async (command, payload) => {
        assert.equal(command, 2030);
        assert.deepEqual(payload.slice(0, 10), [8, 7, ...type2Card.uid, 6]);
        assert.deepEqual(payload.slice(27, 31), target[6]);
        assert.deepEqual(payload.slice(43, 47), [42, 0, 0, 0]);
        writes++;
        if (scenario === 'lost reply') {
          throw new Error('Write outcome unknown');
        }
        return Uint8Array.from([42, 0, 0, 0]);
      });
      const work = writeType2Changes(doc, undefined, () => {}, {
        requestCredential: async () => null,
        confirmChanges: async ({ writes }) => {
          assert.equal(writes.length, 1);
          assert.equal(writes[0].index, 6);
          return scenario !== 'cancel';
        }
      });
      if (['stale page', 'different tag', 'lost reply', 'locked'].includes(scenario)) {
        await assert.rejects(work, /0\/1 pages written and verified/);
        assert.equal(doc.type2Read, undefined);
      } else {
        await work;
        assert.deepEqual(doc.type2Read.memory[6], scenario === 'write' ? [42, 0, 0, 0] : target[6]);
      }
      assert.equal(writes, Number(['write', 'lost reply'].includes(scenario)));
      assert(ended);
    }));
}

for (const [type, security] of [
  [1100, [40, 41, 42]],
  [1101, [130, 131, 132]],
  [1102, [226, 227, 228]],
  [1104, [40, 41, 42, 43]],
  [1105, [16, 17]],
  [1106, [36, 37, 38]],
  [1107, [16, 17]],
  [1108, [36, 37, 38]]
]) {
  test(`Type 2 change preflight reads only changed pages and current security (${type})`, (t) =>
    runtime(async () => {
      const doc = await blankType2(type, type2Card.uid);
      doc.type2Read = { type, card: structuredClone(doc.card), memory: structuredClone(doc.memory) };
      const target = structuredClone(doc.memory);
      doc.memory[6][0] ^= 1;
      doc.memory[7][0] ^= 1;
      const reads = [];
      let ended = false;
      let confirmed = false;
      t.mock.method(Device.prototype, 'begin', async function () {
        this.card = structuredClone(type2Card);
        return this.card;
      });
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'writeCommand', async () =>
        assert.fail('Cancelled preview must not write')
      );
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        if (command === 2031) {
          assert.equal(type, 1104);
          assert.equal(payload[10], 0);
          reads.push([payload[9]]);
          return Uint8Array.from([...target[payload[9]], ...Array(12).fill(255)]);
        }
        assert.equal(command, 2028);
        if (payload[0] === 0) {
          return Uint8Array.from(doc.geometry.version);
        }
        if (payload[0] === 5) {
          const pages = Array.from(
            { length: payload.at(-1) - payload[1] + 1 },
            (_, index) => payload[1] + index
          );
          assert(pages.length <= 8);
          reads.push(pages);
          return Uint8Array.from(pages.flatMap((page) => target[page]));
        }
        assert.equal(payload[0], 1);
        reads.push([payload[1]]);
        // Other READ pages are intentionally unreliable at an access boundary.
        return Uint8Array.from([...target[payload[1]], ...Array(12).fill(255)]);
      });
      await writeType2Changes(doc, undefined, () => {}, {
        requestCredential: async () => assert.fail('Unprotected target must not ask for a key'),
        confirmChanges: async ({ writes }) => {
          confirmed = true;
          assert.deepEqual(
            writes.map((unit) => unit.index),
            [6, 7]
          );
          return false;
        }
      });
      assert(confirmed && ended);
      assert.deepEqual(
        reads.flat().sort((left, right) => left - right),
        [2, 6, 7, ...security]
      );
      if ([1100, 1101, 1102].includes(type)) {
        assert.equal(reads.length, 3);
      }
      assert.deepEqual(doc.type2Read.memory, target);
    }));
}

for (const status of [2, 3, 7, 0x60, 0x69]) {
  test(`Type 2 preflight falls back from FAST_READ without trusting wrapped pages (${status})`, (t) =>
    runtime(async () => {
      const doc = await blankType2(1101, type2Card.uid);
      doc.type2Read = {
        type: doc.type,
        card: structuredClone(doc.card),
        memory: structuredClone(doc.memory)
      };
      const target = structuredClone(doc.memory);
      doc.memory[6][0] ^= 1;
      let fast = 0;
      const reads = [];
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async function () {
        this.card = structuredClone(type2Card);
        return this.card;
      });
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'writeCommand', async () =>
        assert.fail('Cancelled preview must not write')
      );
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        assert.equal(command, 2028);
        if (payload[0] === 0) {
          return Uint8Array.from(doc.geometry.version);
        }
        if (payload[0] === 5) {
          fast++;
          throw Object.assign(new Error('FAST_READ rejected'), { status });
        }
        assert.equal(payload[0], 1);
        reads.push(payload[1]);
        return Uint8Array.from([...target[payload[1]], ...Array(12).fill(255)]);
      });
      let confirmed = false;
      await writeType2Changes(doc, undefined, () => {}, {
        requestCredential: async () => assert.fail('Target is unprotected'),
        confirmChanges: async () => {
          confirmed = true;
          return false;
        }
      });
      assert(confirmed && ended);
      assert.equal(fast, 1);
      assert.deepEqual(reads, [2, 6, 130, 131, 132]);
    }));
}

for (const failure of ['truncated', 'transport', 'cancelled', 'different model']) {
  test(`Type 2 fast preflight refuses an incomplete check (${failure})`, (t) =>
    runtime(async () => {
      const doc = await blankType2(1101, type2Card.uid);
      doc.type2Read = {
        type: doc.type,
        card: structuredClone(doc.card),
        memory: structuredClone(doc.memory)
      };
      doc.memory[6][0] ^= 1;
      const controller = new AbortController();
      let ended = false;
      t.mock.method(Device.prototype, 'begin', async function () {
        this.card = structuredClone(type2Card);
        return this.card;
      });
      t.mock.method(Device.prototype, 'end', async () => {
        ended = true;
      });
      t.mock.method(Device.prototype, 'writeCommand', async () =>
        assert.fail('Incomplete checks must not write')
      );
      t.mock.method(Device.prototype, 'command', async (command, payload) => {
        assert.equal(command, 2028);
        if (payload[0] === 0) {
          return Uint8Array.from(
            failure === 'different model' ? [0, 4, 4, 2, 1, 0, 0x0f, 3] : doc.geometry.version
          );
        }
        if (failure === 'transport') {
          throw new Error('Connection lost');
        }
        if (failure === 'cancelled') {
          controller.abort();
          return Uint8Array.from([0, 0, 0, 0]);
        }
        return Uint8Array.from([0]);
      });
      await assert.rejects(
        writeType2Changes(doc, controller.signal, () => {}, {
          requestCredential: async () => assert.fail('Invalid reads must not request authentication'),
          confirmChanges: async () => assert.fail('Incomplete checks must not reach confirmation')
        }),
        /0\/1 pages written and verified/
      );
      assert(ended);
      assert.equal(doc.type2Read, undefined);
    }));
}

for (const type of [1101, 1104]) {
  for (const rejected of [false, true]) {
    test(`Targeted Type 2 checks preserve authentication boundaries (${type}, rejected=${rejected})`, (t) =>
      runtime(async () => {
        const doc = await blankType2(type, type2Card.uid);
        doc.type2Read = { type, card: structuredClone(doc.card), memory: structuredClone(doc.memory) };
        const target = structuredClone(doc.memory);
        const ultralightC = type === 1104;
        if (ultralightC) {
          target[42][0] = 4;
        } else {
          target[131][3] = 4;
        }
        doc.memory[6][0] ^= 1;
        const key = Array(ultralightC ? 16 : 4).fill(0x42);
        let authenticatedReads = 0;
        let prompts = 0;
        let confirmed = false;
        let ended = false;
        t.mock.method(Device.prototype, 'begin', async function () {
          this.card = structuredClone(type2Card);
          return this.card;
        });
        t.mock.method(Device.prototype, 'end', async () => {
          ended = true;
        });
        t.mock.method(Device.prototype, 'writeCommand', async () => assert.fail('Preview must not write'));
        t.mock.method(Device.prototype, 'command', async (command, payload) => {
          if (command === 2028) {
            assert(!ultralightC);
            if (payload[0] === 0) {
              return Uint8Array.from(doc.geometry.version);
            }
            if (payload[1] >= 4) {
              throw Object.assign(new Error('Protected'), { status: 2 });
            }
            return Uint8Array.from(
              payload[0] === 5 ? target[payload[1]] : [...target[payload[1]], ...Array(12).fill(255)]
            );
          }
          assert.equal(command, 2031);
          const page = payload[9];
          assert(page < (ultralightC ? 44 : 133));
          if (payload[10]) {
            assert.equal(payload[10], ultralightC ? 4 : 3);
            assert.deepEqual(payload.slice(11, 11 + key.length), key);
            authenticatedReads++;
            if (rejected) {
              throw Object.assign(new Error('Authentication rejected'), { status: 6 });
            }
          } else if (page >= 4) {
            throw Object.assign(new Error('Protected'), { status: 2 });
          }
          return Uint8Array.from([...target[page], ...Array(12).fill(255)]);
        });
        const work = writeType2Changes(doc, undefined, () => {}, {
          requestCredential: async (needsUltralightC) => {
            prompts++;
            assert.equal(needsUltralightC, ultralightC);
            return key;
          },
          confirmChanges: async () => {
            confirmed = true;
            return false;
          }
        });
        if (rejected) {
          await assert.rejects(work, /Authentication rejected/);
          assert.equal(authenticatedReads, 1);
          assert.equal(doc.type2Read, undefined);
        } else {
          await work;
          assert(authenticatedReads > 0);
        }
        assert.equal(confirmed, !rejected);
        assert.equal(prompts, 1);
        assert(ended);
      }));
  }
}

test('NTAG215 targeted changes detect newly set dynamic locks', (t) =>
  runtime(async () => {
    const doc = await blankType2(1101, type2Card.uid);
    doc.type2Read = { type: doc.type, card: structuredClone(doc.card), memory: structuredClone(doc.memory) };
    const target = structuredClone(doc.memory);
    target[130][0] = 1;
    doc.memory[17][0] ^= 1;
    let ended = false;
    t.mock.method(Device.prototype, 'begin', async function () {
      this.card = structuredClone(type2Card);
      return this.card;
    });
    t.mock.method(Device.prototype, 'end', async () => {
      ended = true;
    });
    t.mock.method(Device.prototype, 'writeCommand', async () =>
      assert.fail('Locked pages must not be written')
    );
    t.mock.method(Device.prototype, 'command', async (command, payload) => {
      assert.equal(command, 2028);
      if (payload[0] === 0) {
        return Uint8Array.from(doc.geometry.version);
      }
      assert.equal(payload[0], 5);
      return Uint8Array.from(target.slice(payload[1], payload.at(-1) + 1).flat());
    });
    await assert.rejects(
      writeType2Changes(doc, undefined, () => {}, {
        requestCredential: async () => null,
        confirmChanges: async () => assert.fail('Locked pages must not reach confirmation')
      }),
      /locked or its write permissions are unknown/
    );
    assert(ended);
    assert.equal(doc.type2Read, undefined);
  }));
