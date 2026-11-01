import test from 'node:test';
import assert from 'node:assert/strict';
import { SlotDevice } from '../src/device/slots.mjs';
import { withDevice } from '../src/device/transport.mjs';

for (const status of [null, 409]) {
  test(`slot heartbeats distinguish network loss from an expired lease (${status})`, async (t) => {
    let heartbeat;
    let submitted = 0;
    t.mock.method(globalThis, 'setInterval', (callback) => {
      heartbeat = callback;

      return 1;
    });
    t.mock.method(globalThis, 'clearInterval', () => {});
    t.mock.method(globalThis, 'fetch', async (path) => {
      if (path.endsWith('/session')) {
        return Response.json({ session: 1 });
      }

      if (path.startsWith('/api/keepalive')) {
        if (status) {
          return new Response(null, { status });
        }
        throw new TypeError('Lost reply');
      }

      if (path.startsWith('/api/slots/')) {
        submitted++;
      }

      return new Response(new Uint8Array([0, 104]));
    });

    const device = new SlotDevice();
    await withDevice(device, async () => {
      await device.begin();
      heartbeat();
      await new Promise(setImmediate);
      if (status) {
        await assert.rejects(
          device.action('enable', [0, 2, 0, 0, 0, 1, 1]),
          (error) => error.httpStatus === status
        );
      } else {
        await device.action('enable', [0, 2, 0, 0, 0, 1, 1]);
      }

      assert.equal(submitted, status ? 0 : 1);
    });
  });
}

test('session cleanup preserves the primary failure and reports cleanup-only failures', async () => {
  const primary = new Error('Upload failed');
  const cleanup = new Error('Cleanup failed');
  let ends = 0;
  const device = {
    async end() {
      ends++;
      throw cleanup;
    }
  };

  await assert.rejects(
    withDevice(device, async () => {
      throw primary;
    }),
    (error) => error === primary
  );
  await assert.rejects(
    withDevice(device, async () => 42),
    (error) => error === cleanup
  );
  assert.equal(ends, 2);
  assert.equal(await withDevice({ async end() {} }, async () => 42), 42);
});
