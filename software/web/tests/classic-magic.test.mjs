import test from 'node:test';
import assert from 'node:assert/strict';
import {
  decodeClassicMagic,
  classicMagicSignatures,
  classicMagicEvidence,
  magicGenerationDetected
} from '../src/documents/classic-magic.mjs';

test('magic decoding distinguishes signatures, unfused UID and Gen2 evidence', () => {
  for (const data of [
    [0, 0, 0],
    [0, 1, 0],
    [0, 4, 3],
    [0, 4, 4],
    [0, 0, 5]
  ]) {
    const magic = decodeClassicMagic(Uint8Array.from(data));
    assert.equal(magicGenerationDetected(magic), !!data[1]);
  }
  const unfused = decodeClassicMagic(Uint8Array.of(0, 8, 0));
  assert.equal(magicGenerationDetected(unfused), false);
  assert.deepEqual(classicMagicSignatures(unfused), ['Unfused UID signature']);
  assert.deepEqual(classicMagicSignatures(decodeClassicMagic(Uint8Array.of(0, 4, 3))), ['Gen2 / CUID']);
  assert.match(classicMagicEvidence({ gen2: 3 }), /no data sent/);
});

test('magic decoding rejects old, truncated, unknown and inconsistent replies', () => {
  for (const data of [
    [1],
    [0, 0],
    [0, 0, 0, 0],
    [0, 0x40, 0],
    [0, 3, 0],
    [0, 0x30, 0],
    [0x30, 0, 0],
    [0, 4, 0],
    [0, 0, 3],
    [0, 0, 6]
  ]) {
    assert.throws(() => decodeClassicMagic(Uint8Array.from(data)), /Invalid magic/);
  }
});
