import test from 'node:test';
import assert from 'node:assert/strict';
import { decodeSectorDiagnostic } from './hardware/classic-sector-result.mjs';

function response(count, failed = count, status = 0) {
  const mask = (1 << failed) - 1;
  const data = Array.from({ length: count }, (_, offset) => Array(16).fill(offset < failed ? offset + 1 : 0));
  return [
    0,
    0,
    mask >>> 8,
    mask & 255,
    ...data.flat(),
    1,
    failed === count ? 0xff : failed,
    status >>> 8,
    status & 255
  ];
}

test('sector diagnostics retain complete 4/16-block reads and exact failed-prefix evidence', () => {
  for (const count of [4, 16]) {
    const complete = decodeSectorDiagnostic(response(count), count);
    assert.equal(complete.blockMask, (1 << count) - 1);
    assert.equal(complete.firstFailedOffset, null);
    assert.equal(complete.blockReadStatus, 0);
    for (let failed = 0; failed < count; failed++) {
      for (const error of [1, 2, 3, 4, 5, 6, 7, 0x1234]) {
        const result = decodeSectorDiagnostic(Uint8Array.from(response(count, failed, error)), count);
        assert.equal(result.rfStatus, 0);
        assert.equal(result.blockMask, (1 << failed) - 1);
        assert.equal(result.firstFailedOffset, failed);
        assert.equal(result.blockReadStatus, error);
        assert.deepEqual(
          result.blocks.slice(0, failed),
          Array.from({ length: failed }, (_, i) => Array(16).fill(i + 1))
        );
        assert.deepEqual(result.blocks.slice(failed), Array(count - failed).fill(null));
      }
    }
  }
});

test('selection/authentication failures remain separate and missing firmware support is explicit', () => {
  for (const error of [1, 2, 3, 4, 5, 6, 7]) {
    assert.deepEqual(decodeSectorDiagnostic([0, error], 4), {
      rfStatus: error,
      blockMask: null,
      blockReadStatus: null,
      firstFailedOffset: null,
      blocks: null
    });
  }
  for (const status of [0x60, 0x67, 0x69]) {
    assert.throws(() => decodeSectorDiagnostic([0, status], 4), /firmware/);
  }
  assert.throws(() => decodeSectorDiagnostic([0, 6, 0], 4), /failed sector/);
});

test('sector diagnostics reject incomplete, inconsistent or contaminated evidence', () => {
  const complete = response(4);
  const partial = response(4, 2, 3);
  const invalid = [
    [],
    [0],
    complete.slice(0, -1),
    complete.slice(0, -4),
    [...complete, 0],
    [...complete.slice(0, -4), 2, 255, 0, 0],
    [...complete.slice(0, -4), 1, 255, 0, 3],
    [...partial.slice(0, -4), 1, 4, 0, 3],
    [...partial.slice(0, -4), 1, 2, 0, 0],
    [0, 0, 0, 5, ...partial.slice(4)],
    [0, 0, 1, 15, ...complete.slice(4)],
    [...partial.slice(0, 36), 1, ...partial.slice(37)],
    [NaN, ...complete.slice(1)],
    [256, ...complete.slice(1)]
  ];
  for (const bytes of invalid) {
    assert.throws(() => decodeSectorDiagnostic(bytes, 4));
  }
  assert.throws(() => decodeSectorDiagnostic(complete, 5));
});
