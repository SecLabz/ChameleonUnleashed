import test from 'node:test';
import assert from 'node:assert/strict';
import {
  blankLf,
  editLf,
  encodeLfFields,
  HID_FORMATS,
  lfFields,
  lfIdentity,
  lfSchema
} from '../../../.cache/web/assets/modules/documents/lf.mjs';
import { decodeSlotImage, encodeSlotImage } from '../src/documents/slot-image.mjs';
import { restoreDraft, serializeDraft } from '../../../.cache/web/assets/modules/documents/workspace.mjs';

const cases = [
  [100, { id: '00 01 ab cd 05' }, [0, 1, 171, 205, 5]],
  [170, { id: '0300FF4F' }, [3, 0, 255, 79]],
  [180, { id: '0102030405' }, [1, 2, 3, 4, 5]],
  [150, { id: 'Ab12Cd34' }, [65, 98, 49, 50, 67, 100, 51, 52]],
  [310, { id: '1234abcd' }, [73, 68, 84, 75, 18, 52, 171, 205]],
  [
    104,
    { id: '0102030405', epilogue: '1234567890abcdef' },
    [1, 2, 3, 4, 5, 18, 52, 86, 120, 144, 171, 205, 239]
  ],
  [200, { format: '1', facility: '42', number: '12345' }, [1, 0, 0, 0, 42, 0, 0, 0, 48, 57, 0, 0, 0]]
];

test('RFID fields produce the canonical slot record and survive draft reload', async () => {
  for (const [type, fields, expected] of cases) {
    const doc = blankLf(type);
    assert.throws(() => encodeSlotImage(doc), /complete/);
    assert.equal(editLf(doc, fields), null);
    assert.deepEqual(doc.memory[0], expected);
    assert.equal(doc.lf.input, undefined);
    assert.deepEqual(decodeSlotImage(encodeSlotImage(doc)).memory, [expected]);
    assert.deepEqual((await restoreDraft(serializeDraft(doc))).memory, [expected]);
  }
  assert.equal(lfIdentity(blankLf()), 'ID not set');
});

test('empty and invalid RFID drafts preserve the typed text without retaining a sendable ID', async () => {
  const doc = blankLf(170);
  assert.deepEqual((await restoreDraft(serializeDraft(doc))).memory, [null]);
  editLf(doc, { id: '0300FF4F' });
  const error = editLf(doc, { id: '03X' });
  assert.equal(error.field, 'id');
  assert.deepEqual(doc.memory, [null]);
  assert.throws(() => encodeSlotImage(doc), /complete/);
  const restored = await restoreDraft(serializeDraft(doc));
  assert.deepEqual(lfFields(restored), { id: '03X' });
  assert.deepEqual(restored.memory, [null]);
  assert.equal(editLf(restored, { id: '00000001' }), null);
  assert.equal(lfFields(restored).id, '00000001');
  assert.equal(restored.lf.input, undefined);
});

test('every native HID format uses its actual field limits, including large card numbers', () => {
  for (const [, code, limits] of HID_FORMATS) {
    const fields = { format: String(code) };
    for (const [index, key] of ['facility', 'number', 'issue', 'oem'].entries()) {
      fields[key] = String(limits[index]);
    }
    const data = encodeLfFields(200, fields);
    assert.equal(data.length, 13);
    const doc = blankLf(200);
    doc.memory = [data];
    const decoded = lfFields(doc);
    for (const field of lfSchema(200, fields)) {
      assert.equal(decoded[field.key], fields[field.key]);
      assert.throws(() => encodeLfFields(200, { ...fields, [field.key]: String(field.limit + 1) }), {
        field: field.key
      });
    }
  }
  for (const code of [22, 42]) {
    assert.deepEqual(
      encodeLfFields(200, { format: String(code), facility: '0', number: '0', oem: '0' }).slice(-2),
      [3, 132]
    );
  }
});

test('decimal syntax, PAC ASCII, Electra data and IDTECK payload lengths are validated', () => {
  for (const value of ['-1', '1e2', '0x10', '1.5', '', '999999999999999999999']) {
    assert.throws(() => encodeLfFields(201, { version: '1', facility: '2', number: value }), {
      field: 'number'
    });
  }
  for (const id of ['AB CDEFG', 'é1234567', '1234567', '123456789']) {
    assert.throws(() => encodeLfFields(150, { id }), { field: 'id' });
  }
  assert.throws(() => encodeLfFields(104, { id: '0102030405', epilogue: '' }), { field: 'epilogue' });
  assert.throws(() => encodeLfFields(310, { id: '4944544B1234ABCD' }), { field: 'id' });
});

test('HID identity shows only the fields provided by its format', () => {
  const doc = blankLf(200);
  editLf(doc, { format: '1', facility: '42', number: '12345' });
  assert.equal(lfIdentity(doc), 'FC 42 · Card 12345');
  editLf(doc, { format: '26', number: '34359738367' });
  assert.equal(lfIdentity(doc), 'Card 34359738367');
  const raw = 'FF000000000000000000000000';
  editLf(doc, { format: '255', raw });
  assert.equal(lfIdentity(doc), `Record ${raw}`);
});

test('invalid RFID input and record geometry are rejected when restoring a document', async () => {
  const doc = blankLf(100);
  for (const input of [{ id: 1 }, { extra: 'x' }, []]) {
    doc.lf.input = input;
    await assert.rejects(restoreDraft(serializeDraft(doc)), /LF observation/);
  }
  delete doc.lf.input;
  doc.unitBytes = 4;
  await assert.rejects(restoreDraft(serializeDraft(doc)), /geometry/);
});
