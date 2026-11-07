import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createEngine } from '../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';
import { classicFixture, keyA, keyB, access } from './hardware/classic-fixture.mjs';
import { Device } from '../src/device/device.mjs';
import { setDeviceTransport } from '../src/device/transport.mjs';

const engine = await createEngine(
  await readFile(
    new URL('../../../.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm', import.meta.url)
  )
);

test('the default fixture remains identical to the physically tested Classic 1K image', () => {
  const fixture = classicFixture();
  assert.equal(
    createHash('sha256').update(fixture.bytes).digest('hex'),
    'e641cd2be817f927cd181bc45e611d8b510b8c935bac488532d2faabe2a6d3e9'
  );
});

for (const [size, sak, count] of [
  ['mini', 9, 20],
  ['1k', 8, 64],
  ['2k', 0x10, 128],
  ['4k', 0x18, 256]
]) {
  test(`the ${size} PM3 image agrees with production WASM sector geometry`, () => {
    const fixture = classicFixture(size);
    const geometry = engine({ op: 'classic_geometry', sak });
    assert.equal(fixture.blocks.length, count);
    assert.equal(geometry.blocks, count);
    assert.deepEqual(
      fixture.sectors,
      geometry.sectors.map(({ first, count, trailer }) => ({ first, count, trailer }))
    );
    assert.equal(fixture.bytes.length, 4096);
    assert.deepEqual([...fixture.bytes.subarray(0, count * 16)], fixture.blocks.flat());
    assert(fixture.bytes.subarray(count * 16).every((byte) => byte === 0));
    for (const sector of geometry.sectors) {
      assert.deepEqual(fixture.blocks[sector.trailer], [...keyA, ...access, ...keyB]);
    }
    if (size === '4k') {
      assert.deepEqual(fixture.sectors[32], { first: 128, count: 16, trailer: 143 });
      assert.equal(fixture.sectors[39].trailer, 255);
      assert.notDeepEqual(fixture.blocks[131], [...keyA, ...access, ...keyB]);
    }
  });
}

test('the PM3 2K fixture retains SAK 08 and needs its ATS to select 128 blocks', () => {
  const fixture = classicFixture('2k');
  assert.equal(fixture.model.sak, 8);
  assert.equal(fixture.blocks[0][5], 8);
  assert.equal(engine({ op: 'classic_geometry', sak: fixture.model.sak }).blocks, 64);
  assert.deepEqual(fixture.model.ats, [12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x2f, 0x2f, 1, 0xbc, 0xd6]);
  assert.throws(() => classicFixture('constructor'), /Unsupported PM3 fixture/);
});

for (const mask of [0xffff, 0x8001]) {
  test(`the production reader preserves the last 4K sector and mask ${mask.toString(16)}`, async () => {
    const fixture = classicFixture('4k');
    const geometry = engine({ op: 'classic_geometry', sak: 0x18 });
    const expected = fixture.blocks.slice(240).map((block) => [...block]);
    expected[15] = [...Array(6).fill(0), ...access, ...Array(6).fill(0)];
    const device = new Device();
    device.session = 1;
    device.card = { uid: [0x11, 0x22, 0x33, 0x44] };
    setDeviceTransport({
      async request(path, bytes) {
        assert.equal(path, '/api/command/2027?id=1');
        assert.deepEqual([...bytes], [0x61, 39, ...keyB, 4, 0x11, 0x22, 0x33, 0x44]);
        return Uint8Array.from([0, 0, mask >> 8, mask & 255, ...expected.flat()]);
      }
    });
    try {
      const blocks = await device.readSector(39, keyB, true, geometry);
      assert.equal(blocks.length, 16);
      assert.deepEqual(blocks[0], fixture.blocks[240]);
      assert.deepEqual(blocks[15], expected[15]);
      for (let index = 1; index < 15; index++) {
        assert.deepEqual(blocks[index], mask === 0xffff ? expected[index] : null);
      }
    } finally {
      setDeviceTransport(null);
    }
  });
}
