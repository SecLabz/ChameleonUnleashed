import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { sha256 } from '../src/runtime/sha256.mjs';
import { Device } from '../src/device/device.mjs';
import { CORE_URL, createRuntime } from '../../../.cache/web/assets/modules/runtime/runtime.mjs';
import { TABLE, TABLE_URL } from '../src/runtime/assets.mjs';
import { runAutoPwn } from '../../../.cache/web/assets/modules/recovery/autopwn.mjs';
import { createEngine } from '../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';
import { createClassicKeyCache } from '../src/recovery/classic-key-cache.mjs';
import { classicFixture } from './hardware/classic-fixture.mjs';
import { createKeyReuseFilter } from '../src/recovery/key-reuse.mjs';

const wasm = await readFile(
  new URL('../../../.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm', import.meta.url)
);

const originalFetch = globalThis.fetch;

const workerFactory = () => {
  let engine;
  let closed = false;

  return {
    terminate() {
      closed = true;
    },

    postMessage({ id, wasm, request }) {
      queueMicrotask(async () => {
        try {
          if (wasm) {
            engine = await createEngine(wasm);
          }

          const value = wasm ? null : engine(request);
          if (!closed) {
            this.onmessage({ data: { id, value } });
          }
        } catch (error) {
          if (!closed) {
            this.onmessage({ data: { id, error: error.message } });
          }
        }
      });
    }
  };
};

class Card {
  constructor(missing = -1) {
    this.missing = missing;
    this.ended = false;
    this.reads = 0;
    this.sectorReads = 0;
    this.card = { uid: [1, 2, 3, 4], sak: 8, atqa: [0, 4] };
  }

  async begin() {
    return this.card;
  }

  async probeMagic() {
    assert.fail('Key recovery must not inspect magic capabilities');
  }

  async command(id) {
    assert.equal(id, 2002);

    return [1];
  }

  accepts(_sector, _isKeyB, key) {
    return key.every((byte) => byte === 255);
  }

  async findKey(sector, isKeyB, keys, _geometry, onTested = () => {}) {
    const match = keys.findIndex((key) => this.accepts(sector, isKeyB, key));
    onTested(match < 0 ? keys : keys.slice(0, match + 1));

    return match < 0 ? null : keys[match];
  }

  async readBlock(block) {
    this.reads++;
    if (block === this.missing) {
      const error = new Error('Block unavailable');
      error.status = this.missingStatus ?? 6;
      throw error;
    }

    const data = Array(16).fill(0);
    if (block % 4 === 3) {
      data.splice(0, 16, ...Array(6).fill(255), 255, 7, 128, 105, ...Array(6).fill(255));
    }
    if (block === 0) {
      data.splice(0, 8, 1, 2, 3, 4, 4, 8, 4, 0);
    }

    return data;
  }

  async readSector(sector, key, isKeyB, geometry) {
    this.sectorReads++;
    if (!this.accepts(sector, isKeyB, key)) {
      throw Object.assign(new Error('Authentication rejected'), { status: 6 });
    }
    const { first, count } = geometry.sectors[sector];
    const blocks = Array(count).fill(null);
    for (let i = 0; i < count; i++) {
      try {
        blocks[i] = await this.readBlock(first + i);
      } catch (error) {
        if (error.status !== 6 && error.status !== 2) {
          throw error;
        }
        break;
      }
    }

    return blocks;
  }

  async classicBatch(kind, entries) {
    const sectors = Array.from({ length: 40 }, (_, sector) => ({
      first: sector < 32 ? sector * 4 : 128 + (sector - 32) * 16,
      count: sector < 32 ? 4 : 16
    }));
    const geometry = { sectors };
    const replies = [];
    for (const [address, type, ...key] of entries) {
      const sector = kind ? address : sectors.findIndex((entry) => entry.first === address);
      try {
        if (!kind) {
          const hit = this.accepts(sector, type === 0x61, key) ? key : null;
          replies.push({
            status: hit ? 0 : 6,
            data: Uint8Array.from([Number(!!hit), ...(hit || Array(6).fill(0))])
          });
        } else {
          const blocks = await this.readSector(sector, key, type === 0x61, geometry);
          const mask = blocks.reduce((mask, block, index) => mask | (block ? 1 << index : 0), 0);
          replies.push({
            status: 0,
            data: Uint8Array.from([
              mask >> 8,
              mask & 255,
              ...blocks.flatMap((block) => block || Array(16).fill(0))
            ])
          });
        }
      } catch (error) {
        if (error.status !== 6) {
          throw error;
        }
        replies.push({ status: 6, data: new Uint8Array() });
      }
    }
    return replies;
  }

  async end() {
    this.ended = true;
  }
}

test('CDN core works without SIMD and requests no optional module', async () => {
  const validate = WebAssembly.validate;
  let client;
  const requests = [];
  WebAssembly.validate = () => false;
  globalThis.fetch = async (path) => {
    requests.push(path);

    return new Response(wasm);
  };

  try {
    client = await createRuntime(undefined, workerFactory);

    assert.equal((await client.request({ op: 'classic_geometry', sak: 8 })).blocks, 64);
    assert.deepEqual(requests, [CORE_URL]);
  } finally {
    client?.dispose();
    WebAssembly.validate = validate;
    globalThis.fetch = originalFetch;
  }
});

test('SHA-256 matches independent vectors at padding boundaries', async () => {
  for (const length of [0, 3, 55, 56, 63, 64, 65, 1024, 256 * 1024 + 1]) {
    const input = new Uint8Array(length).map((_, index) => index & 255);

    assert.equal(await sha256(input), createHash('sha256').update(input).digest('hex'));
  }
});

test('CDN resources reject corrupt tables and reuse verified downloads', async (t) => {
  const { getResource, getTables } = await import('../src/runtime/assets.mjs?cdn-tables');
  for (const body of [new Uint8Array(TABLE.size - 1), new Uint8Array(TABLE.size)]) {
    t.mock.method(globalThis, 'fetch', async (url) => {
      assert.equal(url, TABLE_URL);

      return new Response(body);
    });

    await assert.rejects(
      getTables(),
      (error) =>
        /tables unavailable.*Internet/.test(error.message) && /size|SHA|Incomplete/.test(error.cause.message)
    );
  }

  const pack = new Uint8Array([1, 2, 3, 4]);
  const asset = { size: pack.byteLength, sha256: createHash('sha256').update(pack).digest('hex') };
  let requests = 0;
  t.mock.method(globalThis, 'fetch', async (url) => {
    assert.equal(url, TABLE_URL);

    requests++;

    return new Response(pack);
  });

  const tables = await getResource(asset, TABLE_URL);

  assert.deepEqual(new Uint8Array(tables), pack);

  t.mock.method(globalThis, 'fetch', async () => assert.fail('A verified download should be reused'));

  assert.equal(await getResource(asset, TABLE_URL), tables);
  assert.equal(requests, 1);
});

test('optional storage failures allow CDN fallback and cancellation', async () => {
  const originalDB = Object.getOwnPropertyDescriptor(globalThis, 'indexedDB');
  const { getTables } = await import('../src/runtime/assets.mjs?cache-lifecycle');
  let requests = 0;

  try {
    Object.defineProperty(globalThis, 'indexedDB', {
      configurable: true,

      get() {
        throw new Error('SecurityError');
      }
    });
    globalThis.fetch = async () => {
      requests++;
      throw new Error('Server unavailable');
    };

    await assert.rejects(getTables(), /tables unavailable/);
    assert.equal(requests, 1);

    Object.defineProperty(globalThis, 'indexedDB', { configurable: true, writable: true, value: undefined });
    requests = 0;
    globalThis.indexedDB = {
      open() {
        throw new Error('SecurityError');
      }
    };
    globalThis.fetch = async () => {
      requests++;
      throw new Error('Server unavailable');
    };

    await assert.rejects(getTables(), /tables unavailable/);
    assert.equal(requests, 1);

    globalThis.indexedDB = {
      open() {
        return {};
      }
    };

    const controller = new AbortController();
    const pending = getTables(controller.signal);
    controller.abort();

    await assert.rejects(pending, /cancelled/);
    assert.equal(requests, 1);
  } finally {
    if (originalDB) {
      Object.defineProperty(globalThis, 'indexedDB', originalDB);
    } else {
      delete globalThis.indexedDB;
    }
    globalThis.fetch = originalFetch;
  }
});

test('candidate verification observes cancellation between bounded batches', async () => {
  const controller = new AbortController();
  const device = new Device(controller.signal);
  let calls = 0;
  device.session = 1;
  device.command = async (command) => {
    if (command === 2026) {
      return [];
    }
    calls++;
    controller.abort();

    const error = new Error('No match');
    error.status = 6;
    throw error;
  };

  await assert.rejects(
    device.findKey(
      0,
      false,
      Array.from({ length: 100 }, () => [0, 0, 0, 0, 0, 0]),
      { sectors: [{ first: 0 }] }
    ),
    /cancelled/
  );
  assert.equal(calls, 1);
});

test('key verification reuses RF preparation and reopens after idle or another command', async () => {
  const previousPerformance = Object.getOwnPropertyDescriptor(globalThis, 'performance');
  let now = 100;
  const commands = [];
  Object.defineProperty(globalThis, 'performance', { configurable: true, value: { now: () => now } });
  globalThis.fetch = async (path) => {
    const command = Number(path.match(/command\/(\d+)/)?.[1]);
    commands.push(command);

    return new Response(new Uint8Array(command === 2015 ? [0, 6, ...Array(7).fill(0)] : [0, 0x68]));
  };

  const device = new Device();
  device.session = 1;

  const verify = () => device.findKey(0, false, [[0, 0, 0, 0, 0, 0]], { sectors: [{ first: 0 }] });

  try {
    assert.equal(await verify(), null);

    now += 100;

    assert.equal(await verify(), null);

    now += 1100;

    assert.equal(await verify(), null);

    await device.command(2000);

    assert.equal(await verify(), null);
    assert.deepEqual(commands, [2026, 2015, 2015, 2026, 2015, 2000, 2026, 2015]);

    const count = commands.length;

    assert.equal(await device.findKey(0, false, [], {}), null);
    assert.equal(commands.length, count);
  } finally {
    globalThis.fetch = originalFetch;
    Object.defineProperty(globalThis, 'performance', previousPerformance);
  }
});

for (const unsupported of [0x67, 0x69, 404]) {
  test(`key verification rejects firmware without RF sessions (${unsupported})`, async () => {
    const commands = [];
    globalThis.fetch = async (path) => {
      const command = Number(path.match(/command\/(\d+)/)?.[1]);
      commands.push(command);
      if (command === 2026) {
        return unsupported === 404
          ? new Response('', { status: 404 })
          : new Response(new Uint8Array([0, unsupported]));
      }

      return new Response(new Uint8Array([0, 6, ...Array(7).fill(0)]));
    };

    const device = new Device();
    device.session = 1;

    try {
      await assert.rejects(device.findKey(0, false, [[0, 0, 0, 0, 0, 0]], { sectors: [{ first: 0 }] }));
      assert.deepEqual(commands, [2026]);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });
}

test('actual shared WASM recovers keys, reads and exports without magic inspection', async () => {
  globalThis.fetch = async () => new Response(wasm);

  const device = new Card();
  let finalPhase;

  try {
    const result = await runAutoPwn({
      device,
      workerFactory,
      onUpdate: (state) => {
        if (state.complete) {
          finalPhase = state.phase;
        }
      }
    });

    assert.equal(result.summary.resolved_keys, 32);
    assert.equal(result.memory.filter(Boolean).length, 64);
    assert.equal(result.complete, true);
    assert.match(result.dump, /Filetype: Flipper NFC device/);
    assert.equal(finalPhase, 'Read complete');
    assert.equal(device.ended, true);
    assert.equal(device.sectorReads, 16);
    assert.equal(device.reads, 64, 'verified sector data must not be read again for trailer extraction');
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('AutoPwn continues a scan-owned Classic reservation without reacquiring the card', async () => {
  globalThis.fetch = async () => new Response(wasm);

  const device = new Card();
  device.begin = async () => {
    assert.fail('A scan already acquired this card');
  };

  try {
    const result = await runAutoPwn({ device, initialCard: device.card, workerFactory });

    assert.equal(result.memory.filter(Boolean).length, 64);
    assert.equal(result.complete, true);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

for (const [sak, sectors, blocks, missing, rejectedRead = false] of [
  [8, 16, 64, -1],
  [0x18, 40, 256, -1],
  [8, 16, 64, 6],
  [0x18, 40, 256, -1, true]
]) {
  test(`Key A reuse selects bounded read batches only after successful checks (sak=${sak}, missing=${missing}, rejectedRead=${rejectedRead})`, async () => {
    globalThis.fetch = async () => new Response(wasm);
    const device = new Card(missing);
    device.card.sak = sak;
    const geometry = {
      sectors: Array.from({ length: sectors }, (_, sector) => ({
        first: sector < 32 ? sector * 4 : 128 + (sector - 32) * 16,
        count: sector < 32 ? 4 : 16
      }))
    };
    const findKey = device.findKey.bind(device);
    let initialSearches = 0;
    device.findKey = async (sector, isKeyB, keys, ...args) => {
      assert.equal(sector, 0, 'Only the initial dictionary search needs separate key checks');
      assert.equal(isKeyB, false);
      initialSearches++;
      return findKey(sector, isKeyB, keys, ...args);
    };
    const readSector = device.readSector.bind(device);
    let singleReads = 0;
    device.readSector = async (...args) => {
      singleReads++;
      return readSector(...args);
    };
    let batches = 0;
    let rejection = false;
    const checked = new Set([0]);
    device.classicBatch = async (kind, entries) => {
      assert([0, 1].includes(kind));
      assert(entries.length <= 8);
      assert(
        3 + entries.reduce((n, entry) => n + (kind ? 6 + geometry.sectors[entry[0]].count * 16 : 11), 0) <=
          512
      );
      batches++;
      return Promise.all(
        entries.map(async ([sector, type, ...key]) => {
          assert.equal(type, 0x60);
          if (kind === 0) {
            const index = geometry.sectors.findIndex((s) => s.first === sector);
            assert(index >= 0);
            checked.add(index);
            assert(key.every((b) => b === 255));
            return { status: 0, data: Uint8Array.from([1, ...key]) };
          }
          if (rejectedRead) {
            assert(
              checked.has(sector),
              'After a rejected prefetch, future reads must follow successful checks'
            );
            if (!rejection && sector === 1) {
              rejection = true;
              return { status: 6, data: new Uint8Array() };
            }
          }
          const observed = await readSector(sector, key, false, geometry);
          const mask = observed.reduce((mask, block, i) => mask | (block ? 1 << i : 0), 0);
          return {
            status: 0,
            data: Uint8Array.from([
              mask >> 8,
              mask & 255,
              ...observed.flatMap((block) => block || Array(16).fill(0))
            ])
          };
        })
      );
    };
    try {
      const result = await runAutoPwn({ device, workerFactory });
      assert.equal(result.summary.resolved_keys, sectors * 2);
      assert.equal(result.complete, missing === -1);
      assert.equal(result.memory.length, blocks);
      assert.equal(device.sectorReads, sectors);
      assert.equal(
        singleReads,
        rejectedRead ? 2 : 1,
        'Only the anchor and a rejected prefetch need individual reads'
      );
      assert.equal(rejection, rejectedRead);
      assert.equal(initialSearches, 1);
      if (sectors === 16) {
        assert.equal(batches, 4);
      }
      if (missing !== -1) {
        assert.equal(result.memory[missing], null);
        assert(result.memory[4] && result.memory[5] && result.memory[7]);
        assert.equal(result.dump, null);
      }
      assert(device.ended);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });
}

test('rejected reuse keeps lightweight checks and does not prefetch sector reads', async () => {
  globalThis.fetch = async () => new Response(wasm);
  const controller = new AbortController();
  const device = new Card();
  let checks = 0;
  device.classicBatch = async (kind, entries) => {
    assert.equal(kind, 0);
    checks += entries.length;
    return entries.map(() => ({ status: 6, data: new Uint8Array(7) }));
  };
  device.collectNonces = async () => {
    controller.abort();
    throw new Error('Operation cancelled');
  };
  try {
    const result = await runAutoPwn({ device, workerFactory, signal: controller.signal });
    assert(checks > 0);
    assert.equal(device.sectorReads, 1, 'Only the anchor was authenticated successfully');
    assert.equal(result.complete, false);
    assert.equal(result.memory.filter(Boolean).length, 4);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('Stop after a checked read batch retains its data without dispatching the next batch', async () => {
  globalThis.fetch = async () => new Response(wasm);
  const controller = new AbortController();
  const device = new Card();
  const geometry = { sectors: Array.from({ length: 16 }, (_, i) => ({ first: i * 4, count: 4 })) };
  let readBatches = 0;
  device.classicBatch = async (kind, entries) => {
    assert.equal(controller.signal.aborted, false);
    if (!kind) {
      return entries.map(([, , ...key]) => ({ status: 0, data: Uint8Array.from([1, ...key]) }));
    }
    readBatches++;
    const replies = await Promise.all(
      entries.map(async ([sector, , ...key]) => {
        const blocks = await device.readSector(sector, key, false, geometry);
        return { status: 0, data: Uint8Array.from([0, 15, ...blocks.flat()]) };
      })
    );
    controller.abort();
    return replies;
  };
  try {
    const result = await runAutoPwn({ device, workerFactory, signal: controller.signal });
    assert.equal(readBatches, 1);
    assert.equal(result.memory.filter(Boolean).length, 32);
    assert.equal(result.complete, false);
    assert.equal(result.dump, null);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('Classic retry rejects a changed UID before key or memory operations', async () => {
  globalThis.fetch = async () => new Response(wasm);

  const device = new Card();
  device.findKey = async () => assert.fail('Changed card must not be authenticated');

  try {
    await assert.rejects(runAutoPwn({ device, expectedUid: 'AABBCCDD', workerFactory }), /UID changed/);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

for (const missing of [-1, 126]) {
  test(`a known 2K document keeps 128 blocks with an observed SAK 08 (missing=${missing})`, async () => {
    globalThis.fetch = async () => new Response(wasm);
    const device = new Card(missing);
    const ats = [12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x2f, 0x2f, 1, 0xbc, 0xd6];
    try {
      const result = await runAutoPwn({
        device,
        workerFactory,
        expectedUid: '01020304',
        expectedModel: { type: 1002, card: { uid: [1, 2, 3, 4], sak: 8, atqa: [0, 4], ats } }
      });
      assert.equal(result.card.sak, 8, 'the model selector must not rewrite the observed RF identity');
      assert.deepEqual(result.card.ats, ats, 'selection alone must not erase the known ATS');
      assert.equal(result.geometry.blocks, 128);
      assert.equal(result.geometry.sectors.length, 32);
      assert.equal(result.memory.length, 128);
      assert.equal(result.summary.resolved_keys, 64);
      if (missing < 0) {
        assert.equal(result.complete, true);
        assert.match(result.dump, /Mifare Classic type: 2K/);
        assert.match(result.dump, /SAK: 08/);
        assert.match(result.dump, /Block 127:/);
      } else {
        assert.equal(result.memory[126], null);
        assert(result.memory[127]);
        assert.equal(result.complete, false);
        assert.equal(result.dump, null);
      }
      assert(device.ended);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });
}

test('an existing Classic model hint requires the same UID and a compatible observed identity', async () => {
  globalThis.fetch = async () => new Response(wasm);
  try {
    const known = { uid: [1, 2, 3, 4], sak: 8, atqa: [0, 4] };
    for (const options of [
      { expectedModel: { type: 1002, card: known } },
      { expectedModel: { type: 1003, card: { ...known, sak: 24 } }, expectedUid: '01020304' },
      { expectedModel: { type: 1002, card: known }, expectedUid: 'AABBCCDD' },
      { expectedModel: { type: 9999, card: known }, expectedUid: '01020304' },
      { expectedModel: { type: 1002, card: { ...known, atqa: [0, 2] } }, expectedUid: '01020304' }
    ]) {
      const device = new Card();
      device.findKey = async () => assert.fail('An incompatible model must not start recovery');
      const updates = [];
      await assert.rejects(
        runAutoPwn({ device, workerFactory, ...options, onUpdate: (next) => updates.push(next) }),
        /UID changed|model|same UID/
      );
      assert(
        updates.every((next) => !next.geometry),
        'existing memory must not be replaced'
      );
      assert.equal(device.reads, 0);
      assert(device.ended);
    }
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('unread blocks stay partial and cannot export a complete dump', async () => {
  globalThis.fetch = async () => new Response(wasm);

  const device = new Card(4);
  device.missingStatus = 2;

  try {
    const result = await runAutoPwn({ device, workerFactory });

    assert.equal(result.complete, false);
    assert.equal(result.dump, null);
    assert.equal(result.memory[4], null);
    assert(result.memory[5] && result.memory[6]);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('Stop after a partial sector retains its prefix and skips fallback RF', async () => {
  globalThis.fetch = async () => new Response(wasm);

  const device = new Card(6);
  const controller = new AbortController();
  let readsAtStop;

  try {
    const result = await runAutoPwn({
      device,
      workerFactory,
      signal: controller.signal,
      onUpdate: (state) => {
        if (state.phase === 'Reading memory · sector 2/16' && readsAtStop === undefined) {
          readsAtStop = device.reads;
          controller.abort();
        }
      }
    });

    assert(result.memory[4] && result.memory[5]);
    assert.equal(result.memory[6], null);
    assert.equal(device.reads, readsAtStop);
    assert.equal(result.complete, false);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('sector decoder checks short replies and out-of-range block masks', async () => {
  const device = new Device();
  const geometry = { sectors: [{ first: 0, count: 4 }] };
  device.card = { uid: [1, 2, 3, 4] };
  device.command = async (command, payload) => {
    assert.equal(command, 2027);
    assert.deepEqual(payload, [0x60, 0, 1, 2, 3, 4, 5, 6, 4, 1, 2, 3, 4]);

    const bytes = new Uint8Array(66);
    bytes[1] = 3;
    bytes.fill(42, 2, 34);

    return bytes;
  };

  const blocks = await device.readSector(0, [1, 2, 3, 4, 5, 6], false, geometry);

  assert.equal(blocks.filter(Boolean).length, 2);
  assert.equal(blocks[2], null);

  device.command = async () => new Uint8Array(65);

  await assert.rejects(device.readSector(0, [], false, geometry), /sector response/);

  device.command = async () => {
    const bytes = new Uint8Array(66);
    bytes[1] = 16;

    return bytes;
  };

  await assert.rejects(device.readSector(0, [], false, geometry), /block mask/);
});

test('cancellation retains partial results and always ends device ownership', async () => {
  globalThis.fetch = async () => new Response(wasm);

  const device = new Card();
  const controller = new AbortController();

  try {
    const result = await runAutoPwn({
      device,
      workerFactory,
      signal: controller.signal,
      onUpdate: (state) => {
        if (state.memory?.filter(Boolean).length >= 2) {
          controller.abort();
        }
      }
    });

    assert.equal(result.complete, false);
    assert.equal(result.dump, null);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('cancellation aborts a stalled initial runtime download', async () => {
  const controller = new AbortController();
  const device = new Card();
  globalThis.fetch = (_, options) =>
    new Promise((resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(new Error('Aborted')), { once: true });
    });

  try {
    const pending = runAutoPwn({ device, workerFactory, signal: controller.signal });
    controller.abort();

    const result = await pending;

    assert.equal(result.complete, false);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('cancellation terminates a stalled Worker before device ownership', async () => {
  const controller = new AbortController();
  const device = new Card();
  let terminated = false;
  let begun = false;
  device.begin = async () => {
    begun = true;

    return device.card;
  };
  globalThis.fetch = async () => new Response(wasm);

  const stalled = () => ({
    terminate() {
      terminated = true;
    },

    postMessage() {
      controller.abort();
    }
  });

  try {
    const result = await runAutoPwn({ device, workerFactory: stalled, signal: controller.signal });

    assert.equal(result.complete, false);
    assert(terminated);
    assert(!begun);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

function keyCacheFixture() {
  let text = null;
  const storage = {
    writes: 0,
    getItem: () => text,
    setItem(_id, value) {
      storage.writes++;
      text = value;
    },
    removeItem() {
      storage.writes++;
      text = null;
    }
  };
  return { keyCache: createClassicKeyCache(() => storage), storage };
}

test('reuse parity filtering uses the shared WASM and retains matching candidates for authentication', async () => {
  const engine = await createEngine(wasm);
  const key = [0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f];
  const wrong = Array(6).fill(255);
  let captures = 0;
  const filter = createKeyReuseFilter({
    uid: () => [0x53, 0xbf, 0x9c, 0x43],
    call: async (request) => engine(request),
    collect: async () => {
      captures++;
      return Array(2).fill({ nt1: 0x85430819, ciphertext: 0xaf9a42f6, parity: [1, 1, 0, 0] });
    }
  });
  const summary = {
    prng: 'Weak',
    mapping: [
      { key_a: wrong, key_b: key },
      { key_a: Array(6).fill(0), key_b: null }
    ]
  };
  const target = { sector: 1, is_key_b: true };
  assert.equal(await filter.test(summary, target, wrong), false);
  assert.equal(await filter.test(summary, target, key), true);
  assert.equal(captures, 1);
});

for (const [prng, badPrefix] of [
  ['Weak', false],
  ['Hard', false],
  ['Weak', true],
  ['Hard', true]
]) {
  test(`AutoPwn screens reuse and preserves physical fallback (${prng}, badPrefix=${badPrefix})`, async () => {
    const device = new Card();
    const keyA = Array(6).fill(255);
    const keysB = Array.from({ length: 16 }, (_, sector) => Array(6).fill(sector + 1));
    // Sector 15 reuses sector 2's key, exercising a positive nonce match.
    keysB[15] = keysB[2];
    const same = (a, b) => a.every((byte, i) => byte === b[i]);
    const expected = (sector, isKeyB) => (isKeyB ? keysB[sector] : keyA);
    const physical = new Set();
    device.command = async () => [prng === 'Weak' ? 1 : 2];
    device.accepts = (sector, isKeyB, key) => {
      physical.add(`${sector}:${isKeyB}:${key.join(',')}`);
      return same(key, expected(sector, isKeyB));
    };
    const read = device.readBlock.bind(device);
    device.readBlock = async (block) => {
      const bytes = await read(block);
      if (block % 4 === 3) {
        bytes.splice(6, 10, 0x7f, 7, 0x88, 0x69, ...Array(6).fill(0));
      }
      return bytes;
    };
    const captures = new Map();
    device.collectNonces = async (_source, target, count) => {
      const id = `${target.sector}:${target.is_key_b}`;
      captures.set(id, (captures.get(id) || 0) + count);
      return Array.from({ length: count }, () => ({
        nt1: target.sector,
        ciphertext: Number(target.is_key_b),
        parity: [0, 0, 0, 0]
      }));
    };
    let localMisses = 0;
    const simulatedWorker = () => {
      const worker = workerFactory();
      const post = worker.postMessage.bind(worker);
      worker.postMessage = (message) => {
        const request = message.request;
        if (request?.op === 'autopwn_submit' && request.result.Sweep?.physically_tested === false) {
          localMisses++;
        }
        if (request?.op !== 'filter_dictionary') {
          return post(message);
        }
        const observation = request.observations[0];
        const key = expected(observation.nt1, !!observation.ciphertext);
        const candidates =
          badPrefix && observation.nt1 === 15 ? [] : request.keys.filter((candidate) => same(candidate, key));
        queueMicrotask(() =>
          worker.onmessage({
            data: { id: message.id, value: { candidates, next_count: 0, exhausted: true } }
          })
        );
      };
      return worker;
    };
    globalThis.fetch = async () => new Response(wasm);
    try {
      const result = await runAutoPwn({
        device,
        workerFactory: simulatedWorker,
        keyCache: keyCacheFixture().keyCache,
        dictionary: keysB
          .map((key) => key.map((byte) => byte.toString(16).padStart(2, '0')).join(''))
          .join('\n')
      });
      assert(result.complete);
      assert(localMisses > 20, 'local exclusions are not reported as failed RF authentications');
      for (let sector = 0; sector < 16; sector++) {
        assert.deepEqual(result.summary.mapping[sector].key_b, keysB[sector]);
        assert(
          physical.has(`${sector}:true:${keysB[sector].join(',')}`),
          'each recovered key is physically checked'
        );
      }
      assert(
        [...captures.entries()]
          .filter(([id]) => id.endsWith(':true') && (!badPrefix || id !== '15:true'))
          .every(([, count]) => count <= (prng === 'Weak' ? 2 : 3)),
        'recovery consumes the cached prefix'
      );
      assert(device.ended);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });
}

test('remembered keys survive a new read session and avoid searches without reusing memory', async () => {
  const { keyCache, storage } = keyCacheFixture();
  globalThis.fetch = async () => new Response(wasm);
  try {
    const first = await runAutoPwn({ device: new Card(), workerFactory, keyCache });
    assert(first.complete);
    assert.equal(storage.writes, 1, 'discoveries are saved once at the end');
    const device = new Card();
    device.findKey = async () => assert.fail('Known sectors need no dictionary search');
    device.command = async () => assert.fail('A complete cache needs no PRNG probe');
    const read = device.readBlock.bind(device);
    device.readBlock = async (block) => {
      const bytes = await read(block);
      if (block === 1) {
        bytes[0] = 42;
      }
      return bytes;
    };
    const result = await runAutoPwn({ device, workerFactory, keyCache });
    assert(result.complete);
    assert.equal(result.memory[1][0], 42, 'every read fetches fresh tag memory');
    assert.equal(device.reads, 64, 'readable Key B must not be used for authentication');
    assert.equal(storage.writes, 1, 'an unchanged cache is not rewritten');
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('rejected cached keys fall back to recovery and are replaced after the read', async () => {
  const { keyCache, storage } = keyCacheFixture();
  const geometry = { blocks: 64, sectors: Array(16).fill(null) };
  const old = Array.from({ length: 16 }, () => ({ key_a: Array(6).fill(42), key_b: null }));
  const device = new Card();
  keyCache.save(device.card, geometry, old);
  storage.writes = 0;
  let searches = 0;
  const find = device.findKey.bind(device);
  device.findKey = (...args) => {
    searches++;
    assert.equal(args[2].length, new Set(args[2].map((key) => key.join(','))).size);
    return find(...args);
  };
  globalThis.fetch = async () => new Response(wasm);
  try {
    const result = await runAutoPwn({ device, workerFactory, keyCache, dictionary: 'FFFFFFFFFFFF' });
    assert(result.complete);
    assert(searches > 0);
    assert.equal(storage.writes, 1);
    assert(keyCache.load(device.card, geometry).every((keys) => keys.key_a.every((b) => b === 255)));
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('stopping recovery preserves discoveries with a single cache write', async () => {
  const { keyCache, storage } = keyCacheFixture();
  const controller = new AbortController();
  const device = new Card();
  globalThis.fetch = async () => new Response(wasm);
  try {
    const result = await runAutoPwn({
      device,
      workerFactory,
      keyCache,
      signal: controller.signal,
      onUpdate(next) {
        assert.equal(storage.writes, 0, 'storage is never written during recovery');
        if (next.summary?.resolved_keys) {
          controller.abort();
        }
      }
    });
    assert.equal(result.complete, false);
    assert.equal(storage.writes, 1);
    assert(keyCache.load(device.card, { blocks: 64, sectors: Array(16) })[0].key_a);
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

for (const batchFailure of [null, 0x67, 0x69, 404, 1]) {
  test(`remembered sector reads handle batch support and card loss (${batchFailure})`, async () => {
    const { keyCache, storage } = keyCacheFixture();
    const device = new Card();
    const geometry = {
      blocks: 64,
      sectors: Array.from({ length: 16 }, (_, sector) => ({
        first: sector * 4,
        count: 4,
        trailer: sector * 4 + 3
      }))
    };
    keyCache.save(
      device.card,
      geometry,
      geometry.sectors.map(() => ({
        key_a: Array(6).fill(255),
        key_b: Array(6).fill(255),
        origin_b: 'Trailer'
      }))
    );
    const before = storage.getItem();
    storage.writes = 0;
    let batches = 0;
    device.classicBatch = async (kind, entries) => {
      batches++;
      assert.equal(kind, 1);
      assert(entries.length <= 7);
      if (batchFailure) {
        throw Object.assign(
          new Error('Batch unavailable'),
          batchFailure === 404 ? { httpStatus: 404 } : { status: batchFailure }
        );
      }
      return Promise.all(
        entries.map(async ([sector, type, ...key]) => {
          assert.equal(type, 0x60, 'readable B is never an authentication source');
          const blocks = await device.readSector(sector, key, false, geometry);
          return { status: 0, data: Uint8Array.from([0, 15, ...blocks.flat()]) };
        })
      );
    };
    device.findKey = async () => assert.fail('Cached keys need no search');
    globalThis.fetch = async () => new Response(wasm);
    try {
      if (batchFailure) {
        await assert.rejects(runAutoPwn({ device, workerFactory, keyCache }), /Batch unavailable/);
        assert.equal(storage.getItem(), before, 'failed commands must not invalidate remembered keys');
      } else {
        const result = await runAutoPwn({ device, workerFactory, keyCache });
        assert(result.complete);
        assert.equal(batches, 3);
        assert.equal(device.reads, 64);
      }
      assert.equal(storage.writes, 0);
      assert(device.ended);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });
}

for (const [size, sak, type] of [
  ['2k', 8, 1002],
  ['4k', 24, 1003]
]) {
  test(`cached A/B keys read the entire ${size} geometry in bounded batches`, async () => {
    const fixture = classicFixture(size);
    const card = { ...fixture.model, uid: fixture.blocks[0].slice(0, 4), sak };
    const device = new Card();
    device.card = card;
    const checked = new Set();
    let blocksRead = 0;
    device.findKey = async () => assert.fail('Every cached key is verified by its read');
    device.command = device.findKey;
    device.classicBatch = async (kind, entries) => {
      assert(entries.length <= 8);
      assert(
        3 +
          entries.reduce(
            (size, [sector]) => size + (kind ? 6 + fixture.sectors[sector].count * 16 : 11),
            0
          ) <=
          512
      );
      return entries.map(([address, keyType, ...key]) => {
        const sector = kind ? address : fixture.sectors.findIndex((group) => group.first === address);
        const { first, count, trailer } = fixture.sectors[sector];
        const offset = keyType === 0x61 ? 10 : 0;
        assert.deepEqual(key, fixture.blocks[trailer].slice(offset, offset + 6));
        checked.add(`${sector}:${keyType}`);
        if (!kind) {
          return { status: 0, data: Uint8Array.from([1, ...key]) };
        }
        blocksRead += count;
        const blocks = fixture.blocks.slice(first, first + count).map((block) => [...block]);
        blocks[count - 1].fill(0, 0, 6);
        blocks[count - 1].fill(0, 10, 16);
        return {
          status: 0,
          data: Uint8Array.from([count === 16 ? 255 : 0, count === 16 ? 255 : 15, ...blocks.flat()])
        };
      });
    };
    const geometry = {
      blocks: fixture.sectors.reduce((sum, s) => sum + s.count, 0),
      sectors: fixture.sectors
    };
    const keys = fixture.sectors.map(({ trailer }) => ({
      key_a: fixture.blocks[trailer].slice(0, 6),
      key_b: fixture.blocks[trailer].slice(10, 16)
    }));
    const { keyCache } = keyCacheFixture();
    keyCache.save(card, geometry, keys);
    globalThis.fetch = async () => new Response(wasm);
    try {
      const result = await runAutoPwn({
        device,
        workerFactory,
        keyCache,
        expectedUid: card.uid
          .map((byte) => byte.toString(16).padStart(2, '0'))
          .join('')
          .toUpperCase(),
        expectedModel: { type, card }
      });
      assert(result.complete);
      assert.equal(result.memory.length, geometry.blocks);
      assert.equal(checked.size, keys.length * 2);
      assert.equal(blocksRead, geometry.blocks, 'Key B checks must not reread complete sectors');
      assert.deepEqual(result.memory.at(-1), fixture.blocks[fixture.sectors.at(-1).trailer]);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });
}

test('cancelling a cached read exposes only freshly verified keys and preserves the rest of the cache', async () => {
  const { keyCache, storage } = keyCacheFixture();
  const device = new Card();
  const geometry = { blocks: 64, sectors: Array(16).fill(null) };
  keyCache.save(
    device.card,
    geometry,
    Array.from({ length: 16 }, () => ({
      key_a: Array(6).fill(255),
      key_b: null
    }))
  );
  const before = storage.getItem();
  const controller = new AbortController();
  globalThis.fetch = async () => new Response(wasm);
  try {
    const result = await runAutoPwn({
      device,
      workerFactory,
      keyCache,
      signal: controller.signal,
      onUpdate(next) {
        if (next.memory?.some(Boolean)) {
          controller.abort();
        }
      }
    });
    assert.equal(result.summary.resolved_keys, 7);
    assert.equal(result.memory.filter(Boolean).length, 28);
    assert.equal(result.memory[3][0], 255, 'RF-masked bytes are restored only from the verified key');
    assert.equal(result.summary.mapping[7].key_a, null);
    assert.equal(
      storage.getItem(),
      before,
      'untried cached sectors are retained without being treated as verified'
    );
    assert(device.ended);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('cached B checks do not read memory twice', async () => {
  const device = new Card();
  const block = device.readBlock.bind(device);
  device.readBlock = async (index) => {
    const bytes = await block(index);
    if (index % 4 === 3) {
      bytes.splice(6, 4, 0x7f, 7, 0x88, 0x69);
    }
    return bytes;
  };
  const { keyCache } = keyCacheFixture();
  keyCache.save(
    device.card,
    { blocks: 64, sectors: Array(16) },
    Array.from({ length: 16 }, () => ({
      key_a: Array(6).fill(255),
      key_b: Array(6).fill(255)
    }))
  );
  globalThis.fetch = async () => new Response(wasm);
  try {
    const result = await runAutoPwn({ device, workerFactory, keyCache });
    assert(result.complete);
    assert.equal(device.reads, 64);
    assert.equal(device.sectorReads, 16);
    assert.equal(result.summary.resolved_keys, 32);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
