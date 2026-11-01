import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import {
  desfireDocument,
  inspectDesfire,
  encodeDesfireSlot,
  decodeDesfireSlot,
  desfireImage,
  importDesfireImage
} from '../src/documents/desfire.mjs';
import { readDesfire } from '../src/device/desfire-reader.mjs';

const fixture = JSON.parse(
  await readFile(new URL('../../../resource/tools/tests/desfire-public.json', import.meta.url), 'utf8')
);

test('DESFire observations require the canonical version in both sections', () => {
  for (const version of [0, 2, 255]) {
    const capture = structuredClone(fixture);
    capture.metadata[4] = version;
    capture.memory[4] = version;
    assert.throws(() => inspectDesfire(capture), /Unsupported DESFire capture/);
  }
});

test('public observations roundtrip slots and shared codec images without filling denied files', () => {
  const document = desfireDocument(fixture);
  const capture = inspectDesfire(fixture);

  assert.equal(capture.applications.length, 1);
  assert.equal(capture.applications[0].files[1].data_state, 1);
  assert.deepEqual(capture.applications[0].files[1].data, []);
  assert.deepEqual(decodeDesfireSlot(encodeDesfireSlot(document), 'test').capture, fixture);
  assert.deepEqual(importDesfireImage(desfireImage(document), 'test').capture, fixture);

  for (let length = 0; length < fixture.metadata.length; length++) {
    assert.throws(() => inspectDesfire({ ...fixture, metadata: fixture.metadata.slice(0, length) }));
  }
  for (let length = 0; length < fixture.memory.length; length++) {
    assert.throws(() => inspectDesfire({ ...fixture, memory: fixture.memory.slice(0, length) }));
  }
});

test('large captures can be retained while slots reject them before upload', () => {
  const capture = JSON.parse(JSON.stringify(fixture));
  const index = capture.memory.indexOf(64, 19);

  // Replace the first public file with 4608 bytes; retain all later observations.
  capture.memory[index] = 0;
  capture.memory[index + 1] = 18;

  const dataLengthOffset = index + 4;
  capture.memory.splice(dataLengthOffset, 4, 0, 18, 0, 0);
  capture.memory.splice(dataLengthOffset + 4, 64, ...Array(4608).fill(0x42));

  assert.equal(inspectDesfire(capture).applications[0].files[0].data.length, 4608);
  assert.throws(() => encodeDesfireSlot(desfireDocument(capture)), /4500-byte/);
});

test('reader retains public data and denied files with bounded record-unit reads', async () => {
  const capture = inspectDesfire(fixture);
  const application = capture.applications[0];
  const calls = [];
  const device = {
    command: async (id, payload) => {
      assert.equal(id, 2035);

      calls.push(payload);
      if (payload[0] === 1) {
        return new Uint8Array([
          fixture.uid.length,
          ...fixture.uid,
          0x44,
          3,
          0x20,
          capture.card.ats.length,
          ...capture.card.ats
        ]);
      }

      const command = payload[1];
      const parameters = payload.slice(2);
      let data = [];
      if (command === 0x60) {
        data = capture.version;
      } else if (command === 0x6e) {
        data = [0, 6, 0];
      } else if (command === 0x45) {
        data = [15, 0x81];
      } else if (command === 0x64) {
        data = [1];
      } else if (command === 0x6a) {
        data = application.aid;
      } else if (command === 0x6f) {
        data = application.files.map((file) => file.id);
      } else if (command === 0xf5) {
        data = application.files.find((file) => file.id === parameters[0]).settings;
      } else if (command === 0xbd || command === 0xbb) {
        data = application.files.find((file) => file.id === parameters[0]).data;
      }

      return new Uint8Array([0, ...data]);
    }
  };

  const document = await readDesfire(device, capture.card);

  assert.equal(document.capture.complete, true);
  assert.equal(inspectDesfire(document.capture).applications[0].files[1].data_state, 1);

  const recordRead = calls.find((payload) => payload[1] === 0xbb);

  assert.deepEqual(recordRead, [2, 0xbb, 3, 0, 0, 0, 3, 0, 0]);
  assert(!calls.some((payload) => [0x0a, 0xaa, 0x71].includes(payload[1])));
});

test('interruption preserves previous files and leaves the interrupted file unread', async () => {
  const capture = inspectDesfire(fixture);
  const application = capture.applications[0];
  const device = {
    command: async (id, payload) => {
      if (payload[0] === 1) {
        return new Uint8Array([7, ...fixture.uid, 0x44, 3, 0x20, 6, ...capture.card.ats]);
      }

      const command = payload[1];
      if (command === 0xbb) {
        throw new Error('Operation cancelled');
      }

      const values = {
        0x60: capture.version,
        0x6e: [0, 6, 0],
        0x45: [15, 0x81],
        0x64: [0],
        0x6a: application.aid,
        0x5a: [],
        0x6f: [1, 2, 3]
      };
      let data = values[command];
      if (command === 0xf5) {
        data = application.files.find((file) => file.id === payload[2]).settings;
      } else if (command === 0xbd) {
        data = application.files[0].data;
      }

      return new Uint8Array([0, ...data]);
    }
  };

  const document = await readDesfire(device, capture.card);
  const files = inspectDesfire(document.capture).applications[0].files;

  assert.equal(document.capture.complete, false);
  assert.equal(files[0].data.length, 64);
  assert.equal(files[2].data_state, 3);
});
