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

  async findKey(_sector, _isKeyB, keys, _geometry, onTested = () => {}) {
    const match = keys.findIndex((key) => key.every((byte) => byte === 255));
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
    if (!key.every((byte) => byte === 255)) {
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
  test(`key verification supports firmware without RF sessions (${unsupported})`, async () => {
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
      for (let i = 0; i < 2; i++) {
        assert.equal(await device.findKey(0, false, [[0, 0, 0, 0, 0, 0]], { sectors: [{ first: 0 }] }), null);
      }

      assert.deepEqual(commands, [2026, 2015, 2015]);
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
