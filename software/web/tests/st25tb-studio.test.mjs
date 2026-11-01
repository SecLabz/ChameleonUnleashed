import test from 'node:test';
import assert from 'node:assert/strict';
import { planSt25tb } from '../src/documents/st25tb-plan.mjs';

const uid = [0xd0, 2, 0x33, 1, 2, 3, 4, 5];

const capture = () => ({
  uid,
  memory: Array.from({ length: 16 }, () => [0, 0, 0, 0]),
  system: [255, 255, 255, 255]
});

const blankDocument = () => ({
  type: 2000,
  kind: 'st25tb',
  card: { uid, system: [255, 255, 255, 255] },
  memory: Array.from({ length: 16 }, () => [0, 0, 0, 0])
});

test('the write plan separates normal blocks, increasing counters and OTP', () => {
  const document = blankDocument();
  const physical = capture();
  physical.memory[5] = [0xfd, 0xff, 0xff, 0xff];
  document.memory[1] = [1, 2, 3, 4];
  document.memory[5] = [0xfe, 0xff, 0xff, 0xff];
  document.card.system = [0xfe, 0xff, 0xff, 0xff];

  const plan = planSt25tb(document, physical);

  assert.deepEqual(
    plan.writes.map((item) => item.block),
    [1]
  );
  assert.deepEqual(
    plan.tearoffs.map((item) => item.block),
    [5]
  );
  assert.equal(plan.otp.block, 255);
  assert.throws(() => planSt25tb(document, { ...physical, uid: [1, ...uid.slice(1)] }), /identity/);

  document.card.system = [255, 255, 255, 255];
  physical.system = [0xfe, 255, 255, 255];

  assert.throws(() => planSt25tb(document, physical), /1 to 0/);
});
