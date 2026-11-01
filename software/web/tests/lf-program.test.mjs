import test from 'node:test';
import assert from 'node:assert/strict';
import {
  decodeLfChip,
  inspectLfChip,
  lfChipInfo,
  prepareLfCopy,
  programLfCopy
} from '../src/device/lf-program.mjs';
import { scanLf } from '../src/device/lf-reader.mjs';
import { restoreDraft, serializeDraft } from '../../../.cache/web/assets/modules/documents/workspace.mjs';

const source = {
  name: 'Source',
  kind: 'lf',
  type: 100,
  unitBytes: 5,
  card: { uid: [], atqa: [], sak: 0, ats: [] },
  memory: [[1, 2, 3, 4, 5]],
  lf: { clock: 32 }
};
const before = [0x00148041, 0xff800000, 0x0034a800, 0, 0, 0, 0];
const desired = [0x00088041, 0x11223344, 0x55667788];

test('chip details distinguish a programming candidate from unsupported protection', () => {
  assert.deepEqual(lfChipInfo(before), {
    modulation: 'Manchester',
    clock: 64,
    passwordEnabled: false,
    programmingSupported: true
  });
  assert.equal(lfChipInfo(null), null);
  assert.equal(lfChipInfo(undefined), null);
  for (const protection of [0x10, 0x100, 0x200, 0x20000]) {
    const info = lfChipInfo([before[0] | protection, ...before.slice(1)]);
    assert.equal(info.programmingSupported, false);
    assert.equal(info.passwordEnabled, protection === 0x10);
  }
});

function snapshot(blocks = before, flags = 0) {
  const bytes = new Uint8Array(36);
  bytes.set([1, 1, 127, flags]);
  const view = new DataView(bytes.buffer);
  blocks.forEach((value, index) => view.setUint32(4 + index * 4, value));
  return bytes;
}

function reply(verified = false) {
  const bytes = new Uint8Array(69);
  bytes.set(snapshot(verified ? [...desired, 0, 0, 0, 0] : before, verified ? 2 : 0));
  bytes[36] = desired.length;
  desired.forEach((value, index) => new DataView(bytes.buffer).setUint32(37 + index * 4, value));
  bytes.set([1, 2, 3, 4], 65);
  return bytes;
}

test('LF preparation binds source clock and fingerprint; programming checks final blocks', async () => {
  const calls = [];
  const device = {
    check() {},
    async command(command, data) {
      calls.push([command, [...data]]);
      return reply(data[1] === 2);
    }
  };
  const original = structuredClone(source);
  const plan = await prepareLfCopy(device, source);
  assert.equal(plan.changes, 3);
  await programLfCopy(device, plan);
  assert.deepEqual(source, original);
  assert.deepEqual(calls[0], [3033, [1, 1, 0, 0, 0, 0, 0, 0, 100, 32, 5, 1, 2, 3, 4, 5]]);
  assert.deepEqual(calls[1], [3033, [1, 2, 0, 0, 0, 0, 0, 0, 100, 32, 5, 1, 2, 3, 4, 5, 1, 2, 3, 4]]);
});

test('inconclusive detection stays distinct from malformed or partial detection', async () => {
  assert.equal(await inspectLfChip({ command: async () => new Uint8Array([1, ...Array(35).fill(0)]) }), null);
  for (const bytes of [
    new Uint8Array(),
    new Uint8Array([1, 1, 1, ...Array(33).fill(0)]),
    new Uint8Array(36)
  ]) {
    assert.throws(() => decodeLfChip(bytes), /T55xx/);
  }
  await assert.rejects(prepareLfCopy({ command: async () => snapshot() }, source), /preparation/);
});

test('lost replies and verification failures never trigger a second write', async () => {
  const plan = await prepareLfCopy({ command: async () => reply() }, source);
  for (const failure of ['lost', 'stale', 'mismatch']) {
    let calls = 0;
    const device = {
      check() {},
      async command() {
        calls++;
        if (failure === 'lost') {
          throw new Error('Disconnected');
        }
        if (failure === 'stale') {
          throw Object.assign(new Error('Target changed'), { status: 0x76 });
        }
        return reply(false);
      }
    };
    await assert.rejects(
      programLfCopy(device, plan),
      failure === 'stale' ? /Target changed/ : /Read the tag/
    );
    assert.equal(calls, 1);
  }
});

test('blank T55xx discovery and recorded chip details survive a draft without claiming an ID', async () => {
  const doc = await scanLf({
    command: async (command) => {
      if (command === 3033) {
        return snapshot();
      }
      throw Object.assign(new Error('Not found'), { status: 0x41 });
    }
  });
  assert.equal(doc.type, undefined);
  assert.deepEqual(doc.memory, []);
  const restored = await restoreDraft(serializeDraft(doc));
  assert.deepEqual(restored.lf.chip, before);
  assert.deepEqual(restored.memory, []);
  doc.lf.chip[0] = -1;
  await assert.rejects(restoreDraft(serializeDraft(doc)), /LF observation/);
});
