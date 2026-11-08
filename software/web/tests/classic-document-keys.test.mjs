import test from 'node:test';
import assert from 'node:assert/strict';
import { classicDocumentKeys, classicDictionaryKeys } from '../src/documents/classic-document-keys.mjs';

const keyA = [0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5];
const keyB = [0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5];
const card = { uid: [1, 2, 3, 4], sak: 8, atqa: [0, 4] };

function partialDocument() {
  return {
    kind: 'classic',
    card: structuredClone(card),
    geometry: { sectors: [{ trailer: 3 }, { trailer: 7 }] },
    memory: Array(8).fill(null),
    recoveredKeys: [
      { key_a: keyA, key_b: null },
      { key_a: keyA, key_b: keyB }
    ]
  };
}

test('dictionary retains recovered keys without memory and deduplicates without mutating the document', () => {
  const document = partialDocument();
  const before = structuredClone(document);
  assert.deepEqual(classicDictionaryKeys(document), ['A0A1A2A3A4A5', 'B0B1B2B3B4B5']);
  assert.deepEqual(document, before);
});

test('each complete memory key overrides recovery while unknown bytes fall back independently', () => {
  const document = partialDocument();
  document.recoveredKeys[0].key_b = keyB;
  document.memory[3] = [...Array(6).fill(null), 0xff, 7, 0x80, 0x69, ...Array(6).fill(0)];
  document.memory[7] = [...keyB, ...Array(10).fill(null)];
  assert.deepEqual(classicDocumentKeys(document), [
    { key_a: keyA, key_b: Array(6).fill(0) },
    { key_a: keyB, key_b: keyB }
  ]);
  assert.deepEqual(classicDictionaryKeys(document), ['A0A1A2A3A4A5', '000000000000', 'B0B1B2B3B4B5']);
});

test('imported memory supplies keys without recovery metadata, including large 4K sectors', () => {
  const document = partialDocument();
  delete document.recoveredKeys;
  document.geometry.sectors[1].trailer = 143;
  document.memory = Array(144).fill(null);
  document.memory[143] = [...keyA, ...Array(4).fill(null), ...keyB];
  assert.deepEqual(classicDocumentKeys(document)[0], { key_a: null, key_b: null });
  assert.deepEqual(classicDictionaryKeys(document), ['A0A1A2A3A4A5', 'B0B1B2B3B4B5']);
});

test('unknown, malformed and non-Classic keys never become dictionary entries', () => {
  const document = partialDocument();
  document.memory[3] = [1, 2];
  document.memory[7] = [...Array(6).fill(256), ...Array(10).fill(null)];
  document.recoveredKeys = [
    { key_a: Array(6).fill(null), key_b: [1, 2] },
    { key_a: Array(6).fill(-1), key_b: Array(6).fill('ff') }
  ];
  for (const value of [document, { ...partialDocument(), kind: 'type2' }, null]) {
    assert.deepEqual(classicDictionaryKeys(value), []);
  }
});

test('scan credentials belong only to sector zero of the matching Classic identity', () => {
  const document = {
    kind: 'identity',
    card: structuredClone(card),
    classicObservation: {
      card: structuredClone(card),
      credentials: [{ key: keyB, keyB: true }]
    }
  };
  assert.deepEqual(classicDocumentKeys(document), [{ key_a: null, key_b: keyB }]);
  assert.deepEqual(classicDictionaryKeys(document), ['B0B1B2B3B4B5']);
  document.card.uid[0] = 2;
  assert.deepEqual(classicDictionaryKeys(document), []);
  document.classicObservation.card = structuredClone(document.card);
  document.card.sak = 0;
  document.classicObservation.card.sak = 0;
  assert.deepEqual(classicDictionaryKeys(document), []);
});

test('RF-masked unknown keys are omitted, while a recovered all-zero key remains valid', () => {
  const document = partialDocument();
  document.memory[3] = [...keyA, 0x7f, 7, 0x88, 0x69, ...Array(6).fill(0)];
  document.memory[7] = [...keyA, 0x7f, 7, 0x88, 0x69, ...keyB];
  document.recoveredKeys[1].key_b = null;
  assert.deepEqual(classicDictionaryKeys(document), ['A0A1A2A3A4A5']);
  document.recoveredKeys[0].key_b = Array(6).fill(0);
  assert.deepEqual(classicDictionaryKeys(document), ['A0A1A2A3A4A5', '000000000000']);
});

test('a Classic geometry identified during inspection retains sector-zero credentials before reading', () => {
  const document = partialDocument();
  delete document.recoveredKeys;
  document.classicObservation = {
    card: structuredClone(card),
    credentials: [{ key: keyA, keyB: false }]
  };
  assert.deepEqual(classicDocumentKeys(document), [
    { key_a: keyA, key_b: null },
    { key_a: null, key_b: null }
  ]);
  assert.deepEqual(classicDictionaryKeys(document), ['A0A1A2A3A4A5']);
  document.card.uid[0] ^= 1;
  assert.deepEqual(classicDictionaryKeys(document), []);
});
