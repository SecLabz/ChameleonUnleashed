import test from 'node:test';
import assert from 'node:assert/strict';
import { decodeSlotImage, encodeSlotImage } from '../src/documents/slot-image.mjs';
import { decodeNfcvSlot, encodeNfcvSlot } from '../src/documents/nfcv-slot.mjs';

import { applyHfDefaults, restoreHfDocument } from '../src/documents/hf-image.mjs';

for (const type of [1500, 1501]) {
  const userBlockCount = type === 1500 ? 8 : 28;
  const storedBlockCount = type === 1500 ? 16 : 32;

  function fixture() {
    const raw = Array.from({ length: 32 + storedBlockCount * 5 }, (_, i) => (i * 7) & 255);
    const uidPrefix = type === 1500 ? [224, 4, 3, 0] : [224, 4, 1, 16];
    raw.splice(0, 4, ...uidPrefix);
    raw[11] = type === 1500 ? 31 : 23;
    raw.splice(
      32 + storedBlockCount * 4,
      storedBlockCount,
      ...Array.from({ length: storedBlockCount }, (_, i) => i % 2)
    );

    return raw;
  }

  test(`${type}: legacy record and JSON preserve all bytes, including reserved data`, () => {
    const raw = fixture();
    const document = decodeNfcvSlot(raw, type, 'Test');

    assert.equal(document.memory.length, userBlockCount);

    const oldDraft = JSON.parse(JSON.stringify(document));
    delete oldDraft.type;
    delete oldDraft.emulator.legacy;

    assert.equal(restoreHfDocument(oldDraft).type, type);

    oldDraft.type = type === 1500 ? 1501 : 1500;

    assert.throws(() => restoreHfDocument(oldDraft), /Invalid NFC reader document/);
    assert.deepEqual(encodeNfcvSlot(document), raw);

    const wire = encodeSlotImage(document);
    const restored = restoreHfDocument(JSON.parse(JSON.stringify(document)));

    assert.deepEqual(encodeSlotImage(restored), wire);
    assert.deepEqual(encodeSlotImage(decodeSlotImage(wire)), wire);
    assert.equal(wire.length, raw.length + 5);

    document.memory[0] = [1, 2, 3, 4];
    document.locks[0] = true;

    const changed = encodeNfcvSlot(document);

    assert.deepEqual(
      changed.slice(32 + userBlockCount * 4, 32 + storedBlockCount * 4),
      raw.slice(32 + userBlockCount * 4, 32 + storedBlockCount * 4)
    );
    assert.deepEqual(
      changed.slice(32 + storedBlockCount * 4 + userBlockCount),
      raw.slice(32 + storedBlockCount * 4 + userBlockCount)
    );
    assert.deepEqual(changed.slice(12, 20), raw.slice(12, 20));
  });

  test(`${type}: flags and asymmetric password bytes map explicitly`, () => {
    const raw = fixture();
    for (const [native, shared] of [[1, 1], [2, 2], [4, 8], [16, 4], ...(type === 1500 ? [[8, 16]] : [])]) {
      raw[11] = native;

      const document = decodeNfcvSlot(raw, type, 'Flags');

      assert.equal(document.emulator.metadata[4], shared);
      assert.deepEqual(encodeNfcvSlot(document), raw);
    }

    const document = decodeNfcvSlot(raw, type, 'Secrets');

    assert.deepEqual(document.emulator.secrets.slice(-4), raw.slice(28, 32).reverse());

    if (type === 1500) {
      assert.deepEqual(document.emulator.secrets.slice(0, 4), raw.slice(20, 24).reverse());
    }
    for (const flag of [32, 64]) {
      document.emulator.metadata[4] = flag;

      assert.throws(() => encodeNfcvSlot(document), /password-protected/);
    }
  });

  test(`${type}: physical captures require defaults and complete data`, () => {
    const document = decodeNfcvSlot(fixture(), type, 'Capture');
    delete document.emulator;

    assert.throws(() => encodeNfcvSlot(document), /defaults/);

    applyHfDefaults(document);

    assert.equal(encodeNfcvSlot(document).length, 32 + storedBlockCount * 5);

    document.memory[0] = null;

    assert.throws(() => encodeNfcvSlot(document), /complete/);
  });
}
