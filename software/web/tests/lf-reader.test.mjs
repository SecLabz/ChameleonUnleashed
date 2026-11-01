import test from 'node:test';
import assert from 'node:assert/strict';
import { scanLf } from '../src/device/lf-reader.mjs';
import { encodeSlotImage, decodeSlotImage } from '../src/documents/slot-image.mjs';

test('RFID scans try supported readers and retain native LF emulation bytes', async () => {
  const cases = [
    [3000, 100, 5],
    [3000, 101, 5],
    [3000, 102, 5],
    [3000, 103, 5],
    [3000, 104, 13],
    [3002, 200, 13],
    [3010, 201, 16],
    [3014, 150, 8],
    [3004, 170, 4],
    [3019, 180, 5]
  ];

  for (const [command, type, size] of cases) {
    const calls = [];
    const document = await scanLf({
      async command(actualCommand, payload) {
        calls.push(actualCommand);
        if (actualCommand !== command) {
          throw Object.assign(new Error('Not found'), { status: 0x41 });
        }

        if (actualCommand === 3002 || actualCommand === 3010) {
          assert.deepEqual(payload, [0]);
        }

        const identifier = Array(size).fill(42);
        if (actualCommand === 3000) {
          return new Uint8Array([0, type, ...identifier]);
        }

        if (actualCommand === 3002) {
          return new Uint8Array([...identifier, 0, 0, 0]);
        }

        return new Uint8Array(identifier);
      }
    });

    assert.equal(document.type, type >= 101 && type <= 103 ? 100 : type);
    assert.equal(document.warnings.length, type === 101 || type === 102 ? 1 : 0);
    assert.equal(document.unitBytes, size);
    assert.deepEqual(decodeSlotImage(encodeSlotImage(document)).memory, [Array(size).fill(42)]);
    assert.equal(calls.at(-1), command);
  }
});

test('RFID scanning distinguishes missing tags, malformed responses and transport errors', async () => {
  await assert.rejects(
    scanLf({
      command: async () => {
        throw Object.assign(new Error('Missing'), { status: 0x41 });
      }
    }),
    (error) => error.status === 0x41
  );
  await assert.rejects(scanLf({ command: async () => new Uint8Array([0, 100, 1]) }), /length/);
  await assert.rejects(
    scanLf({
      command: async () => {
        throw new Error('Disconnected');
      }
    }),
    /Disconnected/
  );
});

test('Viking discovery follows only the shorter EM410x probe', async () => {
  const commands = [];
  await scanLf({
    async command(command) {
      commands.push(command);
      if (command === 3004) {
        return new Uint8Array([1, 2, 3, 4]);
      }
      throw Object.assign(new Error('Missing'), { status: 0x41 });
    }
  });

  assert.deepEqual(commands, [3000, 3004]);
});
