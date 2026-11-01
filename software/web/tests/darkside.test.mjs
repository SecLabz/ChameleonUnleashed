import test from 'node:test';
import assert from 'node:assert/strict';
import { Device } from '../src/device/device.mjs';
import { recoverDarkside } from '../src/recovery/darkside.mjs';

const geometry = { sectors: [{ first: 0 }] };
const target = { sector: 0, is_key_b: false };
const capture = [
  0,
  1,
  2,
  3,
  4,
  0x7c,
  0x39,
  0xd2,
  0xe6,
  ...Array(8).fill(0xa0),
  ...Array(8).fill(5),
  ...Array(8).fill(0)
];

test('bounded device capture binds full UID and continues without reinitializing', async () => {
  const device = new Device();
  device.card = { uid: [9, 8, 7, 1, 2, 3, 4] };

  const requests = [];
  device.command = async (id, data) => {
    requests.push([id, data.slice()]);

    return new Uint8Array(requests.length === 1 ? [5] : capture);
  };

  const observation = await device.captureDarkside(target, 0, geometry);

  assert.equal(observation.nt, 0x7c39d2e6);
  assert.deepEqual(requests, [
    [2029, [0x60, 0, 0, 7, ...device.card.uid]],
    [2029, [0x60, 0, 1, 7, ...device.card.uid]]
  ]);

  await device.captureDarkside(target, 1, geometry);

  assert.equal(requests[2][1][2], 2);
});

test('device rejects another card, malformed replies and invalid keystream', async () => {
  const device = new Device();
  device.card = { uid: [1, 2, 3, 4] };

  const invalid = [
    [[4], /Card changed/],
    [[0], /Invalid/],
    [[7], /Invalid/],
    [[5, 0], /Invalid/],
    [[...capture.slice(0, 17), 16, ...capture.slice(18)], /Invalid/],
    [[0, 9, ...capture.slice(2)], /UID mismatch/]
  ];
  for (const [bytes, pattern] of invalid) {
    device.command = async () => new Uint8Array(bytes);

    await assert.rejects(device.captureDarkside(target, 0, geometry), pattern);
  }
  device.command = async () => new Uint8Array([6]);

  assert.equal(await device.captureDarkside(target, 0, geometry), null);
});

test('workflow verifies candidates and advances an unverified capture', async () => {
  const variants = [];
  const verified = [];
  const key = [1, 2, 3, 4, 5, 6];
  const outcome = await recoverDarkside({
    device: {
      captureDarkside: async (t, variant) => {
        variants.push(variant);

        return {};
      }
    },
    target,
    geometry,
    call: async () => ({ candidates: [key] }),
    verify: async (t, keys) => {
      verified.push(keys);

      return verified.length === 2 ? key : null;
    },

    check() {},

    update() {}
  });

  assert.deepEqual(variants, [0, 1]);
  assert.deepEqual(outcome, { Verified: key });
});

test('unsupported web firmware reports a resource limit without hiding other errors', async () => {
  const messages = [];
  const error = Object.assign(new Error('Device HTTP 404'), { httpStatus: 404 });
  const args = {
    device: {
      async captureDarkside() {
        throw error;
      }
    },
    target,
    geometry,

    check() {},

    update({ phase }) {
      messages.push(phase);
    }
  };

  assert.equal(await recoverDarkside(args), 'ResourceLimited');
  assert.match(messages.at(-1), /update the application firmware/);

  error.httpStatus = 500;

  await assert.rejects(recoverDarkside(args), error);
});
