import test from 'node:test';
import assert from 'node:assert/strict';
import { applyNdef, inspectNdef, amiiboInfo, type2Changes } from '../src/documents/type2-content.mjs';

function tag() {
  const memory = Array.from({ length: 45 }, () => [0, 0, 0, 0]);
  memory[3] = [0xe1, 0x10, 0x12, 0];
  memory[4] = [3, 0, 0xfe, 0];
  return { kind: 'type2', type: 1100, geometry: { profile: 'ntag213' }, memory };
}

test('text and URI edits update one TLV without touching manufacturer or configuration pages', () => {
  for (const [kind, text] of [
    ['text', 'Bonjour 👋'],
    ['uri', 'https://www.example.com/é']
  ]) {
    const doc = tag();
    const original = structuredClone(doc.memory);
    doc.memory = applyNdef(doc, kind, text);
    assert.deepEqual(
      inspectNdef(doc).records.map(({ kind, value }) => ({ kind, value })),
      [{ kind, value: text }]
    );
    assert.deepEqual(doc.memory.slice(0, 4), original.slice(0, 4));
    assert.deepEqual(doc.memory.slice(40), original.slice(40));
    doc.memory = applyNdef(doc, 'text', 'short');
    assert.equal(inspectNdef(doc).records[0].value, 'short');
  }
});

test('capacity, malformed records, unformatted and unread memory never mutate the document', () => {
  const doc = tag();
  const original = structuredClone(doc);
  assert.throws(() => applyNdef(doc, 'text', 'a'.repeat(200)), /available/);
  assert.deepEqual(doc, original);
  doc.memory[4] = [3, 0xff, 0xff, 0xff];
  assert.equal(inspectNdef(doc).editable, false);
  doc.memory[3] = null;
  assert.equal(inspectNdef(doc).editable, false);
  doc.memory[3] = [0xf1, 0x10, 0xff, 0xee];
  assert.equal(inspectNdef(doc).editable, false);
});

test('complex NDEF stays inspectable and cannot be replaced by the simple editor', () => {
  const doc = tag();
  const message = [0x91, 1, 4, 0x54, 2, 0x65, 0x6e, 0x61, 0x51, 1, 4, 0x54, 2, 0x65, 0x6e, 0x62];
  [3, message.length, ...message, 0xfe].forEach((byte, index) => {
    doc.memory[4 + Math.floor(index / 4)][index % 4] = byte;
  });
  const original = structuredClone(doc);
  assert.deepEqual(
    inspectNdef(doc).records.map((entry) => entry.value),
    ['a', 'b']
  );
  assert.equal(inspectNdef(doc).editable, false);
  assert.throws(() => applyNdef(doc, 'text', 'overwrite'), /Complex/);
  assert.deepEqual(doc, original);
});

test('Amiibo needs geometry and fixed structure; unread bytes never establish recognition', () => {
  const doc = { kind: 'type2', type: 1101, memory: Array.from({ length: 135 }, () => [0, 0, 0, 0]) };
  doc.memory[0][0] = 4;
  assert.equal(amiiboInfo(doc), null);
  doc.memory[3] = [0xf1, 0x10, 0xff, 0xee];
  doc.memory[4][0] = 0xa5;
  doc.memory[131][3] = 4;
  doc.memory[132][0] = 0x5f;
  assert(amiiboInfo(doc));
  doc.memory[4] = null;
  assert.equal(amiiboInfo(doc), null);
});

test('physical differences exclude protected pages and require a session read', () => {
  const doc = tag();
  assert.equal(type2Changes(doc), null);
  doc.type2Read = { type: doc.type, memory: structuredClone(doc.memory) };
  doc.memory[0][0] = 4;
  doc.memory[4][3] = 1;
  doc.memory[41][0] = 1;
  assert.deepEqual(
    type2Changes(doc).writes.map((unit) => unit.index),
    [4]
  );
  doc.type2Read.memory[4] = null;
  assert.deepEqual(type2Changes(doc).unknown, [4]);
});

test('multiple messages and unread padding retain inspectable records without enabling edits', () => {
  const doc = tag();
  doc.memory = applyNdef(doc, 'text', 'first');
  const first = inspectNdef(doc);
  const message = [3, 8, 0xd1, 1, 4, 0x54, 2, 0x65, 0x6e, 0x62, 0xfe];
  message.forEach((byte, i) => {
    const offset = first.next + i;
    doc.memory[Math.floor(offset / 4)][offset % 4] = byte;
  });
  assert.equal(inspectNdef(doc).editable, false);
  assert.deepEqual(
    inspectNdef(doc).records.map((record) => record.value),
    ['first', 'b']
  );
  doc.memory = applyNdef(tag(), 'text', 'keep me');
  doc.memory[30] = null;
  assert.equal(inspectNdef(doc).editable, false);
  assert.equal(inspectNdef(doc).records[0].value, 'keep me');
});

test('lock control preserves reserved bytes and URL validation never changes the document', () => {
  const doc = tag();
  const original = structuredClone(doc);
  assert.throws(() => applyNdef(doc, 'uri', 'example.com'), /full URL/);
  assert.deepEqual(doc, original);
  doc.memory[4] = [1, 3, 0xa0, 12];
  doc.memory[5] = [0x34, 3, 0, 0xfe];
  assert.equal(inspectNdef(doc).editable, true);
  doc.memory[4][2] = 0x10;
  doc.memory[5][0] = 0x04;
  assert.equal(inspectNdef(doc).editable, false);
  assert.throws(() => applyNdef(doc, 'text', 'reserved'), /Reserved lock-byte/);
});
