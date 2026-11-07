import test from 'node:test';
import assert from 'node:assert/strict';
import { decodeClassicAts } from '../src/device/classic-model.mjs';
import { inspectClassic } from '../../../.cache/web/assets/modules/device/classic-inspect.mjs';

const card = { uid: [1, 2, 3, 4], atqa: [0, 4], sak: 8 };
const plusX = [12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x2f, 0x2f, 1, 0xbc, 0xd6, 0x60, 0xd3];
const legacy = [0, 0, 1, 1, 0, ...Array(6).fill(0)];
const extended = (ats) => Uint8Array.from([...legacy, 1, ats.length, ...ats]);
const options = { modelEvidence: true };

// Independent byte-at-a-time CRC-A vector builder (production uses a bit loop).
function framed(bytes) {
  let crc = 0x6363;
  for (const byte of bytes) {
    let value = byte ^ (crc & 255);
    value ^= (value << 4) & 255;
    crc = (crc >>> 8) ^ (value << 8) ^ (value << 3) ^ (value >>> 4);
    crc &= 65535;
  }
  return [...bytes, crc & 255, crc >>> 8];
}

test('validated Plus SL1 ATS identifies 2K while retaining the real SAK 08', () => {
  assert.deepEqual(framed(plusX.slice(0, -2)), plusX);
  const model = decodeClassicAts(Uint8Array.from(plusX), card);
  assert.equal(model.type, 1002);
  assert.deepEqual(model.ats, plusX.slice(0, -2));
  assert.equal(card.sak, 8);
  const plusS = framed([12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x2f, 0x2f, 0, 0x35, 0xc7]);
  assert.equal(decodeClassicAts(plusS, card).type, 1002);
  for (const [length, atqa] of [
    [7, 0x44],
    [10, 0x84]
  ]) {
    assert.equal(
      decodeClassicAts(plusX, { ...card, uid: Array(length).fill(1), atqa: [0, atqa] }).type,
      1002
    );
  }
});

test('Classic model inference requires matching SL1, UID layout and known ATS history', () => {
  for (const current of [
    { ...card, sak: 0x10 },
    { ...card, sak: 0x18 },
    { ...card, sak: 0x28 },
    { ...card, atqa: [0, 2] },
    { ...card, atqa: [1, 4] },
    { ...card, uid: Array(7).fill(1) },
    { ...card, uid: [1, 2, 3, 4, 5] }
  ]) {
    assert.equal(decodeClassicAts(plusX, current).type, undefined);
  }
  for (const ats of [
    [2, 0],
    [9, 0x78, 0, 0x91, 2, 0xda, 0xbc, 0x19, 0x10],
    // Plus SE 1K / EV1 historical bytes require separate identification.
    [12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x21, 0x30, 0, 0xf6, 0xd1],
    [...plusX.slice(0, 11), 0xd7],
    [12, 0xf5, ...plusX.slice(2, -2)],
    [12, 0x79, ...plusX.slice(2, -2)]
  ]) {
    assert.equal(decodeClassicAts(framed(ats), card).type, undefined);
  }
  assert.deepEqual(decodeClassicAts([], card), { ats: [] });
});

test('Classic ATS rejects malformed TL, truncation, oversized captures and corrupt CRC', () => {
  for (const frame of [
    [1],
    plusX.slice(0, -1),
    framed([11, ...plusX.slice(1, -2)]),
    [...plusX.slice(0, -1), 0],
    framed([65, ...Array(64).fill(0)])
  ]) {
    assert.throws(() => decodeClassicAts(frame, card), /ATS/);
  }
});

test('optional inspection binds raw ATS model evidence without rewriting identity or credentials', async () => {
  const calls = [];
  const observation = await inspectClassic(
    {
      async command(number, payload) {
        calls.push({ number, payload });
        assert.equal(number, 2040);
        assert.equal(payload.at(-1), 1);
        assert.equal(payload.length, 16 + payload[14] * 6);
        return extended(plusX);
      }
    },
    card,
    undefined,
    options
  );
  assert.equal(observation.type, 1002);
  assert.equal(observation.modelEvidence, true);
  assert.deepEqual(observation.ats, plusX.slice(0, -2));
  assert.deepEqual(observation.card, card);
  assert.deepEqual(observation.credentials, []);
  assert.equal(calls.length, 1);
});

test('optional inspection negotiates a rejected old request once and accepts legacy replies', async () => {
  const calls = [];
  const device = {
    async command(number, payload) {
      calls.push({ number, payload });
      if (calls.length === 1) {
        throw Object.assign(new Error('old request format'), { status: 0x60 });
      }
      return Uint8Array.from(legacy);
    }
  };
  const observation = await inspectClassic(device, card, undefined, options);
  assert.equal(observation.type, undefined);
  assert.equal(observation.modelEvidence, undefined);
  assert.equal(calls.length, 2);
  assert.deepEqual(
    calls.map((entry) => entry.number),
    [2040, 2040]
  );
  assert.deepEqual(calls[0].payload.slice(0, -1), calls[1].payload);
  assert.equal(
    (await inspectClassic({ command: async () => Uint8Array.from(legacy) }, card, undefined, options)).type,
    undefined
  );
});

test('empty ATS evidence is distinct from an older firmware without evidence', async () => {
  const observation = await inspectClassic({ command: async () => extended([]) }, card, undefined, options);
  assert.equal(observation.modelEvidence, true);
  assert.equal(observation.type, undefined);
  assert.equal(observation.ats, undefined);
});

test('model evidence errors and cancellation do not issue a fallback request', async () => {
  const invalid = [
    [...legacy, 2, 0],
    [...legacy, 1],
    [...legacy, 1, 14, ...plusX.slice(0, -1)],
    [...legacy, 1, 65, ...Array(65).fill(0)],
    [...legacy, 1, 14, ...plusX.slice(0, -1), 0]
  ];
  for (const reply of invalid) {
    let calls = 0;
    await assert.rejects(
      inspectClassic(
        {
          command: async () => {
            calls++;
            return Uint8Array.from(reply);
          }
        },
        card,
        undefined,
        options
      )
    );
    assert.equal(calls, 1);
  }
  const controller = new AbortController();
  let calls = 0;
  await assert.rejects(
    inspectClassic(
      {
        signal: controller.signal,
        command: async () => {
          calls++;
          controller.abort();
          throw Object.assign(new Error('stopped'), { status: 0x60 });
        }
      },
      card,
      undefined,
      options
    ),
    /stopped/
  );
  assert.equal(calls, 1);
});
