import { hfDocument } from '../src/documents/hf-image.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { Device } from '../src/device/device.mjs';
import { changeTagUid } from '../../../.cache/web/assets/modules/device/tag-write.mjs';
import { writeSt25tb } from '../src/device/st25tb-write.mjs';

const settled = () => new Promise((resolve) => setImmediate(resolve));

function transport(t, respond) {
  const requests = [];
  let heartbeat;
  t.mock.method(globalThis, 'setInterval', (callback) => {
    heartbeat = callback;

    return 1;
  });
  t.mock.method(globalThis, 'clearInterval', () => {});
  t.mock.method(globalThis, 'fetch', async (path, options) => {
    requests.push(path);
    if (path === '/api/session') {
      return new Response('{"session":7}', { headers: { 'Content-Type': 'application/json' } });
    }

    return respond(path, options);
  });

  return {
    requests,
    tick: async () => {
      heartbeat();
      await settled();
    }
  };
}

test('a lost keepalive reply does not block the next session-validated request', async (t) => {
  const { requests, tick } = transport(t, (path) => {
    if (path.startsWith('/api/keepalive')) {
      throw new TypeError('Failed to fetch');
    }

    return new Response(new Uint8Array([0, 0, 42]));
  });
  const device = new Device();

  try {
    await device.reserve();
    await tick();

    assert.deepEqual([...(await device.command(2000))], [42]);
    assert.equal(requests.filter((path) => path.startsWith('/api/command/')).length, 1);
  } finally {
    await device.end();
  }
});

test('an expired lease remains terminal and no command is submitted', async (t) => {
  const { requests, tick } = transport(
    t,
    (path) => new Response(null, { status: path.startsWith('/api/keepalive') ? 409 : 200 })
  );
  const device = new Device();

  try {
    await device.reserve();
    await tick();

    await assert.rejects(device.command(2000), (error) => error.httpStatus === 409);
    assert(!requests.some((path) => path.startsWith('/api/command/')));
  } finally {
    await device.end();
  }
});

test('the device still rejects an expired session after a lost keepalive reply', async (t) => {
  const { requests, tick } = transport(t, (path) => {
    if (path.startsWith('/api/keepalive')) {
      throw new TypeError('Failed to fetch');
    }

    return new Response(null, { status: path.startsWith('/api/command/') ? 409 : 200 });
  });
  const device = new Device();

  try {
    await device.reserve();
    await tick();

    await assert.rejects(device.command(2000), (error) => error.httpStatus === 409);
    assert.equal(requests.filter((path) => path.startsWith('/api/command/')).length, 1);
  } finally {
    await device.end();
  }
});

test('a late keepalive rejection cannot poison a closed session', async (t) => {
  let rejectHeartbeat;
  transport(t, (path) => {
    if (path.startsWith('/api/keepalive')) {
      return new Promise((resolve) => {
        rejectHeartbeat = () => resolve(new Response(null, { status: 409 }));
      });
    }

    return new Response();
  });

  const device = new Device();
  await device.reserve();

  // The callback starts the request but its response remains pending.
  const callback = globalThis.setInterval.mock.calls[0].arguments[0];
  callback();
  await device.end();
  rejectHeartbeat();
  await settled();

  assert.equal(device.error, null);
});

test('a failed command preserves its error and is never replayed', async (t) => {
  const { requests } = transport(t, () => {
    throw new TypeError('Failed to fetch');
  });
  const device = new Device();
  device.session = 7;

  await assert.rejects(
    device.command(2000),
    (error) => error.message === 'Failed to fetch' && error instanceof TypeError
  );
  assert.equal(requests.length, 1);
});

test('a stalled response body is aborted at the existing timeout and never replayed', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });

  const { requests } = transport(t, (_, { signal }) => ({
    ok: true,
    arrayBuffer: () =>
      new Promise((resolve, reject) =>
        signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), {
          once: true
        })
      )
  }));
  const device = new Device();
  device.session = 7;

  const pending = device.command(2000);
  const rejected = assert.rejects(pending, (error) => error.name === 'AbortError');
  await settled();
  t.mock.timers.tick(20000);
  await rejected;

  assert.equal(requests.length, 1);
});

const geometry = {
  sectors: [
    { first: 0, count: 4 },
    { first: 4, count: 4 }
  ]
};

const key = [255, 255, 255, 255, 255, 255];

const sectorReply = () => new Uint8Array([0, 0, 0, 3, ...Array(64).fill(42)]);

test('Classic sector reads send the expected UID and retain the reply during Stop', async (t) => {
  const controller = new AbortController();
  const { requests } = transport(t, (path, { body }) => {
    if (path.startsWith('/api/end')) {
      return new Response();
    }

    assert.equal(path, '/api/command/2027?id=7');
    assert.deepEqual(Array.from(body), [0x60, 0, ...key, 4, 1, 2, 3, 4]);

    controller.abort();

    return new Response(sectorReply());
  });
  const device = new Device(controller.signal);
  device.session = 7;
  device.card = { uid: [1, 2, 3, 4] };

  const blocks = await device.readSector(0, key, false, geometry);

  assert.deepEqual(blocks, [Array(16).fill(42), Array(16).fill(42), null, null]);
  await assert.rejects(device.readSector(1, key, false, geometry), /cancelled/);

  await device.end();

  assert.equal(requests.length, 2);
});

test('Classic read failures never trigger protocol fallback or replay', async (t) => {
  const { requests } = transport(t, () => {
    throw new TypeError('Failed to fetch');
  });
  const device = new Device();
  device.session = 7;
  device.card = { uid: [1, 2, 3, 4] };

  await assert.rejects(device.readSector(0, key, false, geometry), /Failed to fetch/);
  assert.equal(requests.length, 1);
});

test('failed discovery preserves its error when releasing the session also fails', async (t) => {
  transport(t, (path) => {
    if (path.startsWith('/api/end')) {
      throw new Error('Cleanup failed');
    }

    return new Response(new Uint8Array([0, 1]));
  });

  const device = new Device();

  await assert.rejects(device.begin(), (error) => error.status === 1 && error.message === 'Card not found');
  assert.equal(device.session, 0);
});

for (const mode of ['cancel', 'gen1a', 'keyB']) {
  test(`UID operations accept external decisions and release ownership (${mode})`, async (t) => {
    const uid = [5, 6, 7, 8];
    const writes = [];
    const { requests } = transport(t, (path, { body }) => {
      const command = Number(path.match(/command\/(\d+)/)?.[1]);
      let data = [];

      if (command === 2000) {
        data = [4, 1, 2, 3, 4, 4, 0, 8, 0];
      } else if (command === 2032) {
        data = [0, Number(mode === 'gen1a'), 0];
        assert.deepEqual([...body], [4, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 4, 0, 8, 0]);
      } else if (command === 2015) {
        assert.deepEqual([...body], [0, 0x61, 1, ...key]);
        data = [1, ...key];
      } else if (command === 2033) {
        writes.push([...body]);
        data = [4, ...uid];
      } else {
        assert(command === 2026 || path.startsWith('/api/end'));
      }

      return new Response(new Uint8Array([0, 0, ...data]));
    });

    await changeTagUid(
      undefined,
      () => {},
      { card: { uid } },
      {
        requestUid(selection) {
          assert.equal(!!(selection.magic.flags & 1), mode === 'gen1a');
          assert.deepEqual(selection.suggestedUid, uid);
          assert.deepEqual(selection.card.uid, [1, 2, 3, 4]);
          if (mode === 'cancel') {
            return null;
          }

          return { uid, key: mode === 'keyB' ? key : null, keyB: true };
        }
      }
    );

    const suffix = mode === 'keyB' ? [2, ...key] : [];
    const expected = [4, 1, 2, 3, 4, 0, 0, 0, ...uid, 0, 0, 0, ...suffix];
    assert.deepEqual(writes, mode === 'cancel' ? [] : [expected]);
    assert.equal(requests.at(-1), '/api/end?id=7');
  });
}

for (const mode of ['cancel', 'write', 'cancel OTP']) {
  test(`ST25TB decisions preserve verification and final ownership (${mode})`, async (t) => {
    const uid = [0xd0, 2, 0x33, 1, 2, 3, 4, 5];
    const memory = Array.from({ length: 16 }, () => [0, 0, 0, 0]);
    const source = hfDocument({ protocol: 'st25tb', uid, blocks: 16, block_size: 4, chip_id: 0x5a });
    source.memory = memory.map((block) => [...block]);
    source.memory[1] = [1, 2, 3, 4];
    if (mode === 'cancel OTP') {
      source.card.system = [254, 255, 255, 255];
    }

    const writes = [];
    const { requests } = transport(t, (path, { body }) => {
      const command = Number(path.match(/command\/(\d+)/)?.[1]);
      let data;

      if (command === 2305) {
        data = [...uid, 0x5a, 16, 4];
      } else if (command === 2307) {
        data = [1, 2, 0, 0, ...uid, 0, 16, 4, 8, 0, 0, 0, 0x5a, 255, 255, 255, 255];
        data.push(...memory.flatMap((block) => [1, 0, ...block]));
      } else if (command === 2311) {
        assert.deepEqual([...body], [...uid, 1, 0, 0, 0, 0, 1, 2, 3, 4]);
        writes.push(body[8]);
        memory[1] = [...body.slice(13)];
        data = memory[1];
      } else if (command === 2306) {
        data = memory[body[8]];
      } else {
        assert(path.startsWith('/api/end'));
        data = [];
      }

      return new Response(new Uint8Array([0, 0, ...data]));
    });

    const pending = writeSt25tb(source, undefined, () => {}, {
      confirmPlan: () => mode !== 'cancel',
      confirmOtp: () => false,
      tearoff: () => assert.fail('No counter change was requested')
    });

    if (mode === 'cancel OTP') {
      await assert.rejects(pending, /sysOTP write cancelled/);
      assert.deepEqual(source.card.system, [255, 255, 255, 255]);
    } else {
      await pending;
    }

    assert.deepEqual(writes, mode === 'cancel' ? [] : [1]);
    if (mode !== 'cancel') {
      assert.deepEqual(source.memory, memory);
    }
    assert.equal(requests.at(-1), '/api/end?id=7');
  });
}

test('Stop during T55xx programming retains the returned verification reply', async (t) => {
  const controller = new AbortController();
  const { requests } = transport(t, (path) => {
    if (path.startsWith('/api/command/3033')) {
      controller.abort();
      return new Response(new Uint8Array([0, 0x40, 1, 1, 127, 2, ...Array(65).fill(0)]));
    }
    return new Response(new Uint8Array());
  });
  const device = new Device(controller.signal);
  await device.reserve();
  const result = await device.command(3033, [1, 2, 0, 0, 0, 0, 0]);
  assert.equal(result[3], 2);
  assert.equal(controller.signal.aborted, true);
  await device.end();
  assert.equal(requests.filter((path) => path.startsWith('/api/command/3033')).length, 1);
  assert(requests.includes('/api/end?id=7'));
});

test('an unsupported LF configuration is distinct from a missing firmware command', async (t) => {
  let status = 0x69;
  transport(t, () => new Response(new Uint8Array([0, status])));
  const device = new Device();
  device.session = 7;

  await assert.rejects(
    device.command(3033, [1, 1, 0, 0, 0, 0, 0]),
    (error) => error.status === 0x69 && /configuration/.test(error.message)
  );
  status = 0x67;
  await assert.rejects(
    device.command(3033, [1, 0, 0, 0, 0, 0, 0]),
    (error) => error.status === 0x67 && /command/.test(error.message)
  );
});

test('magic probing closes the key lease and encodes full identity with a Key B credential', async (t) => {
  const key = Array(6).fill(255);
  const { requests } = transport(t, (path, { body }) => {
    if (path.includes('/2026?')) {
      assert.deepEqual([...body], [0]);
      return new Response(Uint8Array.of(0, 0x68));
    }
    assert.deepEqual([...body], [7, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0x44, 0, 8, 2, ...key]);
    return new Response(Uint8Array.of(0, 0, 0, 4, 3));
  });
  const device = new Device();
  device.session = 7;
  device.keySessionAt = performance.now();
  const magic = await device.probeMagic({ uid: [1, 2, 3, 4, 5, 6, 7], atqa: [0, 0x44], sak: 8 }, key, true);
  assert.deepEqual(magic, { flags: 4, gen2: 3 });
  assert.equal(device.keySessionAt, 0);
  assert.deepEqual(requests, ['/api/command/2026?id=7', '/api/command/2032?id=7']);
});

test('Stop during a magic probe retains its validated reply without another RF request', async (t) => {
  const controller = new AbortController();
  const { requests } = transport(t, () => {
    controller.abort();
    return new Response(Uint8Array.of(0, 0, 0, 1, 0));
  });
  const device = new Device(controller.signal);
  device.session = 7;
  assert.deepEqual(await device.probeMagic({ uid: [1, 2, 3, 4], atqa: [0, 4], sak: 8 }), {
    flags: 1,
    gen2: 0
  });
  assert(controller.signal.aborted);
  assert.deepEqual(requests, ['/api/command/2032?id=7']);
});

for (const mode of ['A', 'B', 'stale identity', 'inconclusive', 'stop']) {
  test(`UID change rechecks CUID with recovered credentials for the present tag (${mode})`, async (t) => {
    const card = { uid: [1, 2, 3, 4], atqa: [0, 4], sak: 8 };
    const source = {
      card,
      classicObservation: { card: structuredClone(card), magic: { flags: 4, gen2: 3 } },
      recoveredKeys: [{ key_a: [...key], key_b: [1, 2, 3, 4, 5, 6], origin_b: 'Dictionary' }],
      classicRead: { memory: [] }
    };
    if (mode === 'stale identity') {
      source.classicObservation.card.uid[0] = 42;
    }
    const probes = [];
    const controller = new AbortController();
    const { requests } = transport(t, (path, { body }) => {
      const command = Number(path.match(/command\/(\d+)/)?.[1]);
      let data = [];
      if (command === 2000) {
        data = [4, ...card.uid, 4, 0, 8, 0];
      } else if (command === 2032) {
        const kind = body[14];
        probes.push(kind);
        data = [0, 0, 0];
        if (kind) {
          assert.deepEqual([...body.slice(15)], kind === 1 ? key : [1, 2, 3, 4, 5, 6]);
          data = [0, 4, 3];
          if (mode === 'B' && kind === 1) {
            data = [0, 0, 1];
          } else if (mode === 'inconclusive') {
            data = [0, 0, 5];
          }
          if (mode === 'stop') {
            controller.abort();
          }
        }
      } else {
        assert(path.startsWith('/api/end'));
      }
      return new Response(new Uint8Array([0, 0, ...data]));
    });
    const work = changeTagUid(controller.signal, () => {}, source, {
      requestUid({ magic, credential }) {
        assert.notEqual(mode, 'stop', 'Stop must not open a dialog');
        if (mode === 'A' || mode === 'B') {
          assert.deepEqual(magic, { flags: 4, gen2: 3 });
          assert.deepEqual(credential, {
            key: mode === 'A' ? key : [1, 2, 3, 4, 5, 6],
            keyB: mode === 'B'
          });
        } else {
          assert.equal(credential, null);
          assert.equal(magic.flags, 0);
        }
        return null;
      }
    });
    if (mode === 'stop') {
      await assert.rejects(work, /cancelled/);
    } else {
      await work;
    }
    const expected = { A: [0, 1], B: [0, 1, 2], 'stale identity': [0], inconclusive: [0, 1], stop: [0, 1] };
    assert.deepEqual(probes, expected[mode]);
    assert(source.classicRead, 'Cancelling the dialog retains the physical read');
    assert.equal(requests.at(-1), '/api/end?id=7');
  });
}

test('NFC-V password is scoped to authenticated commands and cleared after failed cleanup', async (t) => {
  transport(t, (path, options) => {
    if (path.startsWith('/api/command/2301')) {
      assert.deepEqual([...options.body], [0xe0, 4, 3, 1, 2, 3, 4]);
      return new Response(new Uint8Array([0, 0]));
    }
    if (path.startsWith('/api/command/2000')) {
      assert.deepEqual([...options.body], []);
      return new Response(new Uint8Array([0, 1]));
    }
    throw new Error('USB disconnected');
  });
  const device = new Device();
  await device.reserve();
  device.nfcvPassword = [1, 2, 3, 4];
  await device.command(2301, [0xe0, 4, 3]);
  await assert.rejects(device.command(2000));
  await assert.rejects(device.end());
  assert.equal(device.nfcvPassword, null);
});
