import test, { afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { inspectFirmware, installFirmware, downloadFirmware, loadCatalogue, startApplication } from './update.mjs';
import { beginRequest } from './nordic-package.mjs';
import { sha256 } from './update-sha256.mjs';

const originalFetch = globalThis.fetch;
afterEach(() => { globalThis.fetch = originalFetch; });
const fixture = async (name) => new Uint8Array(await readFile(new URL(name + '.zip', import.meta.url)));

function device(info, { lostReply = false, recoveryCrc = info.recoveryCrc } = {}) {
  let mode = 'application';
  let received = 0;
  let system = 0;
  let committed = false;
  let writes = 0;
  let lost = false;
  const calls = [];
  globalThis.fetch = async (path, options = {}) => {
    const url = new URL(path, 'http://192.168.16.1');
    calls.push({ url, options });
    if (url.pathname === '/api/update') {
      return Response.json({ mode, layout: 2, recovery_crc32: recoveryCrc, version: info.version,
        session: 42, received, size: info.size, valid: committed });
    }
    if (url.pathname === '/api/update/enter') mode = 'recovery';
    else if (url.pathname === '/api/update/begin') {
      assert.deepEqual(options.body, beginRequest(info, Boolean(options.body[0] & 2)));
    } else if (url.pathname === '/api/update/system') {
      const offset = Number(url.searchParams.get('offset'));
      assert.equal(offset, system);
      assert.deepEqual(options.body, info.system.payload.subarray(offset, offset + options.body.length));
      system += options.body.length;
    } else if (url.pathname === '/api/update/write') {
      const offset = Number(url.searchParams.get('offset'));
      assert.equal(url.searchParams.get('session'), '42');
      assert.deepEqual(options.body, info.application.payload.subarray(offset, offset + options.body.length));
      assert.ok(offset === received || (lost && offset + options.body.length === received));
      if (offset === received) { received += options.body.length; writes++; }
      if (lostReply && !lost) { lost = true; throw new TypeError('Lost USB acknowledgement'); }
    } else if (url.pathname === '/api/update/commit') {
      assert.equal(received, info.size);
      if (info.restore) assert.equal(system, info.system.payload.length);
      committed = true;
    } else if (url.pathname === '/api/update/reboot') {
      assert.equal(committed, true); mode = info.restore ? 'restored' : 'application';
    } else throw new Error('Unexpected endpoint ' + url.pathname);
    return new Response(null, { status: 200 });
  };
  return { calls, writes: () => writes };
}

test('standard Nordic ZIP is inspected and installed without a custom firmware format', async () => {
  const bytes = await fixture('normal');
  const info = await inspectFirmware(bytes);
  assert.equal(info.restore, false);
  assert.equal(info.version, 0x1000001);
  const model = device(info, { lostReply: true });
  const result = await installFirmware(bytes);
  assert.equal(result.mode, 'application');
  assert.equal(model.writes(), Math.ceil(info.size / 1024));
  assert.equal(model.calls.filter(({ url }) => url.pathname === '/api/update/commit').length, 1);
});

test('Nordic requirement lists accept S140 anywhere and reject incompatible or malformed lists', async () => {
  for (const name of ['packed', 'unpacked', 'mixed']) {
    const info = await inspectFirmware(await fixture('requirements-' + name));
    assert.equal(info.restore, false);
    assert.equal(info.version, 0x1000001);
  }
  for (const name of ['unsupported', 'truncated', 'overflow']) {
    await assert.rejects(inspectFirmware(await fixture('requirements-' + name)), /S140|Nordic/);
  }
});

test('leaving recovery waits for the actual application even when the reboot reply is lost', async () => {
  let polls = 0;
  let reboots = 0;
  globalThis.fetch = async (path, options = {}) => {
    if (path === '/api/update/reboot') {
      assert.equal(options.method, 'POST');
      reboots++;
      throw new TypeError('USB disconnected during reboot');
    }
    assert.equal(path, '/api/update');
    polls++;
    return Response.json({ mode: polls === 1 ? 'recovery' : 'application', version: 0x1000000 });
  };
  assert.equal((await startApplication()).mode, 'application');
  assert.equal(polls, 2);
  assert.equal(reboots, 1);
});

test('a refused recovery exit does not pretend the application has started', async () => {
  const calls = [];
  globalThis.fetch = async (path) => {
    calls.push(path);
    return new Response(null, { status: 409 });
  };
  await assert.rejects(startApplication(), /busy/);
  assert.deepEqual(calls, ['/api/update/reboot']);
});

test('unchanged complete legacy ZIP restores only after confirmation and transfers system first', async () => {
  const bytes = await fixture('legacy');
  const info = await inspectFirmware(bytes);
  assert.equal(info.restore, true);
  const model = device(info);
  await assert.rejects(installFirmware(bytes), /confirmed/);
  assert.equal(model.calls.filter(({ options }) => options.method === 'POST').length, 0);
  const result = await installFirmware(bytes, { allowRestore: true, wipe: true });
  assert.equal(result.mode, 'restored');
  const begin = model.calls.find(({ url }) => url.pathname === '/api/update/begin');
  assert.equal(begin.options.body[0], 3);
  const firstSystem = model.calls.findIndex(({ url }) => url.pathname === '/api/update/system');
  const firstApp = model.calls.findIndex(({ url }) => url.pathname === '/api/update/write');
  assert.ok(firstSystem >= 0 && firstSystem < firstApp);
});

test('supervisor replacement requires its own confirmation before changing flash', async () => {
  const bytes = await fixture('changed');
  const info = await inspectFirmware(bytes);
  const previous = await inspectFirmware(await fixture('normal'));
  const model = device(info, { recoveryCrc: previous.recoveryCrc });
  await assert.rejects(installFirmware(bytes), /replaces recovery/);
  assert.equal(model.calls.filter(({ options }) => options.method === 'POST').length, 0);
  assert.equal((await installFirmware(bytes, { allowRecoveryUpdate: true })).mode, 'application');
});

test('downloaded ZIP identity and bounded relative path are checked', async () => {
  globalThis.location = { href: 'http://192.168.16.1/' };
  const bytes = await fixture('normal');
  const identity = await sha256(bytes);
  const asset = { size: bytes.length, sha256: identity, path: `files/${identity}.zip` };
  globalThis.fetch = async () => new Response(bytes);
  assert.deepEqual(await downloadFirmware(asset), bytes);
  await assert.rejects(downloadFirmware({ ...asset, path: '../firmware.zip' }), /Invalid/);
  await assert.rejects(downloadFirmware({ ...asset, size: bytes.length - 1 }), /larger/);
  globalThis.fetch = async () => new Response(new Uint8Array(bytes.length));
  await assert.rejects(downloadFirmware(asset), /verification/);
});

test('catalogue accepts both known sources and rejects arbitrary repository identifiers', async () => {
  globalThis.location = { href: 'http://192.168.16.1/' };
  globalThis.fetch = async () => Response.json({ format: 1, sources: [
    { id: 'unleashed', label: 'Unleashed', releases: [] }, { id: 'official', label: 'Official', releases: [] }
  ] });
  assert.equal((await loadCatalogue()).sources.length, 2);
  globalThis.fetch = async () => Response.json({ format: 1, sources: [{ id: 'unknown', releases: [] }] });
  await assert.rejects(loadCatalogue(), /Invalid/);
});

test('malformed catalogue entries and more than five versions are rejected before rendering', async () => {
  const asset = { size: 22, sha256: 'a'.repeat(64), path: `files/${'a'.repeat(64)}.zip` };
  const source = { id: 'unleashed', label: 'Unleashed', releases: [
    { version: 'v1.1.0', layout: 2, packages: { application: asset } }
  ] };
  globalThis.fetch = async () => Response.json({ format: 1, sources: [source] });
  assert.equal((await loadCatalogue()).sources[0].releases.length, 1);
  for (const catalogue of [null,
    { format: 1, sources: [null] },
    { format: 1, sources: [source, source] },
    { format: 1, sources: [{ ...source, releases: [null] }] },
    { format: 1, sources: [{ ...source, releases: [{ version: 'v1.1.0', layout: 2 }] }] },
    { format: 1, sources: [{ ...source, releases: Array.from({ length: 6 }, (_, i) => ({ ...source.releases[0], version: `v1.1.${i}` })) }] }
  ]) {
    globalThis.fetch = async () => Response.json(catalogue);
    await assert.rejects(loadCatalogue(), /Invalid release catalogue/);
  }
});

test('aborting before a transfer does not send firmware', async () => {
  const bytes = await fixture('normal');
  const info = await inspectFirmware(bytes);
  const model = device(info);
  const controller = new AbortController(); controller.abort(new Error('cancelled'));
  await assert.rejects(installFirmware(bytes, { signal: controller.signal }), /cancelled/);
  assert.equal(model.calls.length, 0);
});

test('invalid or oversized archives never reach the device', async () => {
  globalThis.fetch = async () => { throw new Error('must not contact device'); };
  await assert.rejects(installFirmware(new Uint8Array(10)), /Nordic/);
  await assert.rejects(installFirmware(new Uint8Array(4 * 1024 * 1024 + 1)), /Nordic/);
});
