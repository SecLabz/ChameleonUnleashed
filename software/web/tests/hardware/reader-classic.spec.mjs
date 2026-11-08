import { test, expect } from '@playwright/test';
import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { createEngine } from '../../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { startSimulation } from './pm3-simulation.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';
import { uid, keyA, keyB, access, classicFixture } from './classic-fixture.mjs';
import { decodeSectorDiagnostic } from './classic-sector-result.mjs';
import { factoryTrailer } from '../../src/documents/classic-maintenance.mjs';

const run = promisify(execFile);
const deviceURL = 'http://192.168.16.1';
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');

async function classicBench(
  { page, request },
  testInfo,
  {
    writeChanges = false,
    trailerChanges = false,
    copyData = false,
    formatCard = false,
    unknownKey = false,
    rejectTwoK = false,
    size = '1k'
  } = {}
) {
  const executable = process.env.CHAMELEON_PM3_CLIENT;
  const port = process.env.CHAMELEON_PM3_PORT;
  test.skip(!executable || !port, 'Requires an explicitly configured Proxmark3 HF fixture');
  let enabled = writeChanges ? 'CHAMELEON_BLE_TEST_CLASSIC_WRITES' : 'CHAMELEON_BLE_TEST_CLASSIC_READER';
  if (trailerChanges) {
    enabled = 'CHAMELEON_BLE_TEST_CLASSIC_TRAILERS';
  }
  if (copyData) {
    enabled = 'CHAMELEON_BLE_TEST_CLASSIC_COPY';
    expect(size).toBe('2k');
  }
  if (formatCard) {
    enabled = 'CHAMELEON_BLE_TEST_CLASSIC_FORMAT';
    expect(size).toBe('2k');
  }
  if (unknownKey) {
    enabled = 'CHAMELEON_BLE_TEST_CLASSIC_RECOVERY';
  }
  if (rejectTwoK) {
    enabled = 'CHAMELEON_BLE_TEST_CLASSIC_CAPACITY';
    expect(size).toBe('1k');
  }
  if (size !== '1k' && !writeChanges && !trailerChanges && !copyData && !formatCard && !unknownKey) {
    enabled = 'CHAMELEON_BLE_TEST_CLASSIC_SIZES';
  }
  test.skip(process.env[enabled] !== '1', 'Requires Classic stop preflight and aligned HF');
  const prepared = classicFixture(size);
  const { model, sectors } = prepared;
  const wireModel = { 1000: 5, 1001: 1, 1002: 6, 1003: 7 }[model.type];
  const securitySector = size === '2k' ? 31 : 1;
  const securityTrailer = sectors[securitySector].trailer;
  const securityBlock = sectors[securitySector].first;
  const changedUnits = size === '2k' ? [1, 126] : [1, 4];
  const simulationCommand = `hf mf sim --${size} -u 11223344 -i`;
  let classicStopReport;
  let identityCheck;
  if (size !== '1k') {
    test.skip(
      process.env.CHAMELEON_BLE_TEST_CLASSIC_SIZES !== '1',
      'New sizes also require explicit size opt-in'
    );
    expect(
      unknownKey || (size !== '2k' && (writeChanges || trailerChanges || copyData || formatCard)),
      'Only 1K/2K mutation campaigns are prepared'
    ).toBe(false);
    classicStopReport = process.env[`CHAMELEON_PM3_CLASSIC_${size.toUpperCase()}_STOP_REPORT`];
    expect(
      classicStopReport,
      'requires the completed four-fault report for this exact PM3 size'
    ).toBeTruthy();
    // Validate evidence, then reported PM3 identity, before USB or RAM changes.
    identityCheck = await run(
      process.env.PYTHON || 'python3',
      [
        fileURLToPath(new URL('./pm3_classic_qualification.py', import.meta.url)),
        '--live',
        simulationCommand,
        classicStopReport,
        executable,
        port
      ],
      { timeout: 15000 }
    );
  }
  // Long copy/trailer paths retain independent 180-second simulation limits.
  const splitTrailers = trailerChanges && size === '2k';
  const detailedSectors = splitTrailers || formatCard;
  const ordinaryTimeout = copyData || formatCard ? 420000 : 240000;
  test.setTimeout(splitTrailers ? 600000 : ordinaryTimeout);
  page.setDefaultTimeout(15000);
  await mkdir(testInfo.outputDir, { recursive: true });
  if (identityCheck) {
    await writeFile(testInfo.outputPath('pm3-identity-check.json'), identityCheck.stdout);
  }
  const pm3 = async (command, label) => {
    const result = await run(executable, [port, '-c', command], { timeout: 15000, maxBuffer: 4194304 });
    await writeFile(testInfo.outputPath(`${label}.log`), result.stdout + result.stderr);
  };
  const saveRam = async (label) => {
    const path = testInfo.outputPath(label);
    await pm3(`hf mf esave --4k -f ${JSON.stringify(path)}`, label);
    const bytes = await readFile(`${path}.bin`);
    expect(bytes.length).toBe(4096);
    return bytes;
  };
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const slotsBefore = await usbSlotSnapshot(request, deviceURL);
  const originalRam = await saveRam('pm3-before');
  const restoreBin = testInfo.outputPath('pm3-before.bin');
  const recoveryKey = [0xd7, 0xc2, 0xa9, 0xe4, 0x61, 0x35];
  if (unknownKey) {
    const engine = await createEngine(
      await readFile(
        new URL('../../../../.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm', import.meta.url)
      )
    );
    const dictionary = engine({ op: 'dictionary', text: '', anchor: null, profile: 'Extended' });
    expect(dictionary.nonce_keys).not.toContainEqual(recoveryKey);
    for (const pass of dictionary.passes) {
      expect(pass.keys).not.toContainEqual(recoveryKey);
    }
    // The browser gets no dictionary or credential containing this key.
    prepared.blocks[7].splice(0, 6, ...recoveryKey);
    prepared.bytes.set(prepared.blocks.flat());
  }
  const fixtureBin = testInfo.outputPath('fixture.bin');
  await writeFile(fixtureBin, prepared.bytes);
  const saveGattTrace = await traceGatt(page);
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  const usb = [];
  const writes = {
    enabled: writeChanges || trailerChanges || copyData || formatCard,
    trailers: trailerChanges,
    copyData,
    formatCard,
    usb: [],
    ble: []
  };
  let session = 0;
  let stopSimulation;
  let simulationLog = testInfo.outputPath('simulation.log');
  let copyCancellationCleanup;
  const fixturePhaseCleanups = [];
  let captured;
  let qualified = false;
  let cleanup;
  let gattTrace;
  try {
    await pm3(`hf mf eload --4k -f ${JSON.stringify(fixtureBin)}`, 'pm3-load');
    expect(await saveRam('pm3-loaded')).toEqual(prepared.bytes);
    stopSimulation = await startSimulation(executable, port, simulationCommand, simulationLog, {
      restoreBin,
      classicStop: true,
      classicStopReport
    });
    const openUsb = async () => {
      const lease = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
      expect(lease.status()).toBe(200);
      session = (await lease.json()).session;
    };
    const endUsb = async () => {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
      session = 0;
    };
    await openUsb();
    const command = async (number, payload, expected, status = 0) => {
      const response = await request.post(`${deviceURL}/api/command/${number}?id=${session}`, {
        data: Buffer.from(payload)
      });
      expect(response.status()).toBe(200);
      const reply = [...(await response.body())];
      usb.push({ command: number, payload, reply });
      await writeFile(testInfo.outputPath('usb-progress.json'), JSON.stringify(usb, null, 2));
      if (detailedSectors && number === 2027) {
        usb.at(-1).sectorDiagnostic = decodeSectorDiagnostic(reply, sectors[payload[1]].count);
        await writeFile(testInfo.outputPath('usb-progress.json'), JSON.stringify(usb, null, 2));
      }
      expect(reply).toEqual([status >> 8, status & 255, ...expected]);
    };
    const readSector = async (sector, blocks, readableKeyB = false) => {
      const { first, count, trailer } = sectors[sector];
      const expected = blocks.slice(first, first + count).map((block) => [...block]);
      expected[count - 1] = [
        ...Array(6).fill(0),
        ...blocks[trailer].slice(6, 10),
        ...(readableKeyB ? blocks[trailer].slice(10) : Array(6).fill(0))
      ];
      const mask = (1 << count) - 1;
      await command(
        2027,
        [0x60, sector, ...blocks[trailer].slice(0, 6), uid.length, ...uid, ...(detailedSectors ? [1] : [])],
        [mask >> 8, mask & 255, ...expected.flat(), ...(detailedSectors ? [1, 255, 0, 0] : [])]
      );
    };
    const checkMemory = async (blocks, readableKeyB = false) => {
      await openUsb();
      await command(2026, [1], [], 0x68);
      for (let sector = 0; sector < sectors.length; sector++) {
        await readSector(sector, blocks, readableKeyB);
      }
      await endUsb();
    };
    const nextFixturePhase = async (blocks, label) => {
      expect(session).toBe(0);
      await stopSimulation();
      stopSimulation = null;
      const phaseCleanup = JSON.parse(await readFile(`${simulationLog}.cleanup.json`, 'utf8'));
      expect(phaseCleanup).toMatchObject({
        reason: 'owner EOF',
        stopped: true,
        persistentStopPingConfirmed: true,
        ramRestored: true,
        ramSha256: hash(originalRam)
      });
      expect(await saveRam(`pm3-before-${label}`)).toEqual(originalRam);
      await openUsb();
      await command(2000, [], [], 1);
      await endUsb();
      const nextFixture = testInfo.outputPath(`${label}-fixture.bin`);
      const bytes = Buffer.from(prepared.bytes);
      expect(blocks).toHaveLength(model.blocks);
      bytes.set(blocks.flat());
      expect(bytes.length).toBe(4096);
      await writeFile(nextFixture, bytes);
      await pm3(`hf mf eload --4k -f ${JSON.stringify(nextFixture)}`, `pm3-load-${label}`);
      expect(await saveRam(`pm3-loaded-${label}`)).toEqual(bytes);
      fixturePhaseCleanups.push({
        label,
        cleanup: phaseCleanup,
        all4096BytesIndependentlyRestored: true,
        simulationAbsentAfterStop: true,
        reloadedFixtureSha256: hash(bytes)
      });
      await writeFile(
        testInfo.outputPath('fixture-phases.json'),
        JSON.stringify(fixturePhaseCleanups, null, 2)
      );
      simulationLog = testInfo.outputPath(`${label}.log`);
      stopSimulation = await startSimulation(executable, port, simulationCommand, simulationLog, {
        restoreBin,
        classicStop: true,
        classicStopReport
      });
    };
    const writePayload = (block, before, after) => [
      wireModel,
      uid.length,
      ...uid,
      0,
      0,
      0,
      block,
      1,
      ...keyA,
      ...Array(10).fill(0),
      ...before,
      ...after
    ];
    const replacementA = [0xd7, 0xc2, 0xa9, 0xe4, 0x61, 0x35];
    const replacementB = [0xb6, 0xe4, 0x08, 0xc3, 0xf2, 0x7a];
    const changedSecurity = structuredClone(prepared.blocks);
    // Data group 0: A/B read, B-only write. Trailer remains B-writable.
    changedSecurity[securityTrailer] = [...replacementA, 0x7e, 0x17, 0x88, 0x42, ...replacementB];
    const trailerPayload = (kind, check = true) => [
      uid.length,
      ...uid,
      0,
      0,
      0,
      wireModel,
      securitySector,
      kind,
      ...(kind === 1 ? keyA : keyB),
      ...access,
      ...replacementA,
      ...replacementB,
      ...changedSecurity[securityTrailer].slice(6, 10),
      ...(check ? [1] : [])
    ];
    await command(2000, [], [4, ...uid, ...model.atqa, model.sak, 0]);
    await command(2026, [1], [], 0x68);
    if (unknownKey) {
      await command(2015, [4, 0x60, 1, ...keyA], Array(7).fill(0), 6);
    }
    for (let sector = 0; sector < sectors.length; sector++) {
      const { first, trailer } = sectors[sector];
      const sectorKeyA = prepared.blocks[trailer].slice(0, 6);
      await command(2015, [first, 0x60, 1, ...sectorKeyA], [1, ...sectorKeyA]);
      await command(2015, [first, 0x61, 1, ...keyB], [1, ...keyB]);
      await readSector(sector, prepared.blocks);
    }
    if (rejectTwoK) {
      await command(2030, [6, ...writePayload(1, prepared.blocks[1], Array(16).fill(42)).slice(1)], [], 1);
      const wrongTrailer = trailerPayload(2);
      wrongTrailer[8] = 6;
      await command(2034, wrongTrailer, [], 1);
      await command(2038, [6, ...writePayload(0, prepared.blocks[0], prepared.blocks[0]).slice(1), 1], [], 1);
      await command(2039, [0, 3, 6, 4, ...uid, 0, 0, 0, 1, 1, ...keyA, 1], [], 1);
      for (let sector = 0; sector < sectors.length; sector++) {
        await readSector(sector, prepared.blocks);
      }
      writes.usb.push({
        sameUidAtqaSakButOneK: true,
        model6Rejected: [2030, 2034, 2038, 2039],
        allMemoryPreserved: true
      });
    }
    if (trailerChanges) {
      await command(2034, trailerPayload(1), [], 0x75);
      await command(2034, trailerPayload(2), []);
      await readSector(securitySector, prepared.blocks);
      writes.usb.push({
        sector: securitySector,
        deniedKeyAPreflight: true,
        acceptedKeyBPreflight: true,
        noWrite: true
      });
    }
    if (writeChanges) {
      const targetBlock = size === '2k' ? 126 : 1;
      const targetSector = size === '2k' ? 31 : 0;
      const changed = Array.from({ length: 16 }, (_, index) => index ^ 0xa5);
      await command(2030, writePayload(targetBlock, prepared.blocks[targetBlock], changed), changed);
      await readSector(
        targetSector,
        prepared.blocks.map((block, index) => (index === targetBlock ? changed : block))
      );
      await command(
        2030,
        writePayload(targetBlock, prepared.blocks[targetBlock], Array(16).fill(0x11)),
        [],
        0x75
      );
      await command(
        2030,
        writePayload(targetBlock, changed, prepared.blocks[targetBlock]),
        prepared.blocks[targetBlock]
      );
      await readSector(targetSector, prepared.blocks);
      writes.usb.push({ block: targetBlock, writeVerified: true, staleBeforeRejected: true, restored: true });
    }
    await endUsb();
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await page.locator('[data-view-target="scan"]').click();
    await page.locator('#scan').click();
    await expect(page.locator('#studio-document')).toBeVisible();
    await expect(page.locator('#stop')).toBeHidden();
    await expect(page.locator('#status')).toHaveText('Tag identified · Memory not read');
    if (size === '2k') {
      await expect(page.locator('#studio-family')).toHaveText('Classic 2K');
      await expect(page.locator('#memory-status')).toHaveText('0/128 blocks known');
    }
    if (unknownKey) {
      await page.locator('#status').evaluate((element) => {
        const progress = (globalThis.classicRecoveryProgress = []);
        new MutationObserver(() => progress.push({ at: Date.now(), status: element.textContent })).observe(
          element,
          { childList: true, subtree: true, characterData: true }
        );
      });
    }
    await page.locator('#read').click();
    await expect(page.locator('#status')).toHaveText('Read complete', { timeout: 120000 });
    await expect(page.locator('#stop')).toBeHidden();
    await expect(page.locator('#memory-status')).toHaveText(`${model.blocks}/${model.blocks} blocks known`);
    let action = page.locator('#export-json');
    if (!(await action.isVisible())) {
      await page.locator('#export-menu summary').click();
      action = page.locator('#export-document');
    }
    const download = page.waitForEvent('download');
    await action.click();
    const exported = await readFile(await (await download).path());
    await writeFile(testInfo.outputPath('captured.json'), exported);
    captured = JSON.parse(exported.toString()).document;
    expect(captured).toMatchObject({ kind: 'classic', type: model.type, unitBytes: 16 });
    expect(captured.card).toMatchObject({
      uid,
      atqa: [...model.atqa].reverse(),
      sak: model.sak,
      ats: model.ats || []
    });
    expect(captured.memory).toEqual(prepared.blocks);
    expect(captured.recoveredKeys).toHaveLength(sectors.length);
    for (const [sector, keys] of captured.recoveredKeys.entries()) {
      expect(keys).toMatchObject({
        key_a: prepared.blocks[sectors[sector].trailer].slice(0, 6),
        key_b: keyB
      });
    }
    if (unknownKey) {
      expect(captured.recoveredKeys[1].origin_a).toMatch(/^(Nested|StaticNested|Hardnested)$/);
    }
    if (copyData) {
      const memory = page.locator('#memory textarea');
      const lines = (await memory.inputValue()).split('\n');
      const changed = structuredClone(prepared.blocks);
      for (const [index, block] of changedUnits.entries()) {
        changed[block] = Array.from({ length: 16 }, (_, offset) => offset ^ (index ? 0xa5 : 0x5a));
        lines[block] = Buffer.from(changed[block]).toString('hex');
      }
      await memory.fill(lines.join('\n'));
      const decide = async (accepted) => {
        await page.locator('#save-menu summary').click();
        await page.locator('#copy-tag').click();
        const dialog = page.getByRole('dialog', { name: 'Copy to a tag', exact: true });
        await expect(dialog).toBeVisible({ timeout: 60000 });
        await expect(dialog.getByRole('radio', { name: /^Copy data/ })).toBeChecked();
        await expect(dialog).toContainText('2 data blocks to write · 93 already match.');
        await expect(dialog).not.toContainText('Not copied:');
        await dialog.getByRole('button', { name: accepted ? 'Copy data' : 'Cancel', exact: true }).click();
        await expect(page.locator('#stop')).toBeHidden({ timeout: 90000 });
        await expect(page.locator('#status')).toHaveText(
          accepted ? '2 blocks written and verified · data copy' : 'Copy cancelled'
        );
      };
      await decide(false);
      await checkMemory(prepared.blocks);
      writes.ble.push({ copyCancelled: true, allMemoryPreserved: true });
      // Full target comparisons can exceed one simulation's hard deadline.
      // Stop and restore the first fixture before arming a fresh bounded job.
      await stopSimulation();
      stopSimulation = null;
      copyCancellationCleanup = JSON.parse(await readFile(`${simulationLog}.cleanup.json`, 'utf8'));
      expect(copyCancellationCleanup).toMatchObject({
        reason: 'owner EOF',
        stopped: true,
        persistentStopPingConfirmed: true,
        ramRestored: true,
        ramSha256: hash(originalRam)
      });
      expect(await saveRam('pm3-between-copy-phases')).toEqual(originalRam);
      await openUsb();
      await command(2000, [], [], 1);
      await endUsb();
      await pm3(`hf mf eload --4k -f ${JSON.stringify(fixtureBin)}`, 'pm3-copy-reload');
      expect(await saveRam('pm3-copy-reloaded')).toEqual(prepared.bytes);
      simulationLog = testInfo.outputPath('copy-confirmed.log');
      stopSimulation = await startSimulation(executable, port, simulationCommand, simulationLog, {
        restoreBin,
        classicStop: true,
        classicStopReport
      });
      await decide(true);
      await checkMemory(changed);
      writes.ble.push({ copyData: true, blocks: changedUnits, independentUsbReadback: true });
      await openUsb();
      for (const block of changedUnits) {
        await command(
          2030,
          writePayload(block, changed[block], prepared.blocks[block]),
          prepared.blocks[block]
        );
      }
      await endUsb();
      await checkMemory(prepared.blocks);
      writes.usb.push({ copyFixtureRestored: true, allMemoryPreserved: true });
    }
    if (formatCard) {
      const trailerBlocks = sectors.map((sector) => sector.trailer);
      const dataBlocks = prepared.blocks
        .map((_, index) => index)
        .filter((index) => index && !trailerBlocks.includes(index));
      const formatted = prepared.blocks.map((block, index) => {
        if (index === 0) {
          return [...block];
        }
        return trailerBlocks.includes(index) ? factoryTrailer(index === securityTrailer) : Array(16).fill(0);
      });
      const mutations = (trace) =>
        trace.requests.filter(
          (entry) => entry.operation === 8 && entry.argument === 2039 && entry.body[0] === 2
        );
      const decide = async (accepted, alreadyFormatted = false) => {
        await page.locator('#more-menu summary').click();
        await page.locator('#format-tag').click();
        const dialog = page.getByRole('dialog', { name: 'Format MIFARE Classic', exact: true });
        await expect(dialog).toBeVisible();
        const button = dialog.getByRole('button', {
          name: alreadyFormatted ? 'Close' : 'Format card',
          exact: true
        });
        await expect(button).toBeEnabled({ timeout: 90000 });
        await expect(dialog.locator('.maintenance-target')).toHaveText('Classic 2K · UID 11223344');
        await expect(dialog.locator('[data-effect="identity"]')).toHaveText('Preserved');
        await expect(dialog.locator('[data-changes]')).toHaveText(
          alreadyFormatted
            ? 'Card already formatted · no writes needed'
            : '95 data blocks to erase · reset 32 sectors'
        );
        const trace = await saveGattTrace(testInfo.outputPath('format-review-trace.json'));
        expect(mutations(trace)).toHaveLength(alreadyFormatted ? 127 : 0);
        if (alreadyFormatted) {
          await expect(dialog.locator('.maintenance-result')).toContainText(
            'No writes needed. The card has not been modified.'
          );
          await button.click();
        } else {
          await dialog
            .getByRole('button', { name: accepted ? 'Format card' : 'Cancel', exact: true })
            .click();
          await expect(page.locator('#status')).toHaveText(
            accepted
              ? 'Factory format verified · 127 blocks · read again to refresh memory'
              : 'Maintenance cancelled · no writes sent',
            { timeout: 90000 }
          );
        }
        await expect(dialog).toHaveCount(0);
        await expect(page.locator('#stop')).toBeHidden();
      };
      await decide(false);
      await checkMemory(prepared.blocks);
      writes.ble.push({
        formatCancelled: true,
        all128BlocksPreserved: true,
        noMutationsBeforeConfirmation: true
      });
      await nextFixturePhase(prepared.blocks, 'format-confirmed');
      await decide(true);
      await checkMemory(formatted, true);
      writes.ble.push({
        formatted: true,
        dataBlocksErased: dataBlocks,
        sectorsReset: 32,
        all128BlocksIndependentlyVerified: true,
        manufacturerBlockPreserved: true,
        finalSectorGpb: 0xbc,
        readBindingInvalidated: true
      });
      await decide(false, true);
      await checkMemory(formatted, true);
      writes.ble.push({ alreadyFormatted: true, noAdditionalWrites: true, all128BlocksPreserved: true });
      await page.locator('#read').click();
      await page
        .getByRole('dialog', { name: 'Read fresh memory?', exact: true })
        .getByRole('button', { name: 'Recover', exact: true })
        .click();
      await expect(page.locator('#status')).toHaveText('Read complete', { timeout: 90000 });
      await expect(page.locator('#stop')).toBeHidden();
      expect(
        (await page.locator('#memory textarea').inputValue())
          .split('\n')
          .map((line) => [...Buffer.from(line.replace(/\s/g, ''), 'hex')])
      ).toEqual(formatted);
      writes.ble.push({ formattedMemoryReadThroughUi: true, all128BlocksCompared: true });
    }
    if (writeChanges) {
      const memory = page.locator('#memory textarea');
      const write = page.locator('#write-changes');
      const originalText = await memory.inputValue();
      const changed = structuredClone(prepared.blocks);
      changed[1] = Array.from({ length: 16 }, (_, index) => index ^ 0x5a);
      changed[changedUnits[1]] = Array.from({ length: 16 }, (_, index) => index ^ 0xa5);
      const lines = originalText.split('\n');
      for (const block of changedUnits) {
        lines[block] = Buffer.from(changed[block]).toString('hex');
      }
      await memory.fill(lines.join('\n'));
      await expect(write).toHaveText('Write changes (2)');
      const decide = async (accepted, expectedStatus) => {
        await write.click();
        const dialog = page.getByRole('dialog', { name: 'Write changes?', exact: true });
        await expect(dialog).toContainText(`Tag 11223344 · 2 data blocks: ${changedUnits.join(', ')}.`);
        await dialog
          .getByRole('button', { name: accepted ? 'Write changes' : 'Cancel', exact: true })
          .click();
        await expect(page.locator('#status')).toHaveText(expectedStatus);
        await expect(page.locator('#stop')).toBeHidden();
      };
      await decide(false, 'Writing cancelled');
      await checkMemory(prepared.blocks);
      writes.ble.push({ cancelled: true, allMemoryPreserved: true });
      await decide(true, '2 blocks written and verified');
      await expect(write).toHaveText('Write changes (0)');
      await expect(write).toBeDisabled();
      await checkMemory(changed);
      writes.ble.push({ blocks: changedUnits, writtenAndVerified: true, independentUsbReadback: true });
      await memory.fill(originalText);
      await expect(write).toHaveText('Write changes (2)');
      await decide(true, '2 blocks written and verified');
      await checkMemory(prepared.blocks);
      writes.ble.push({ blocks: changedUnits, restored: true, allMemoryPreserved: true });
      const external = Array.from({ length: 16 }, (_, index) => index ^ 0x3c);
      await openUsb();
      await command(2030, writePayload(1, prepared.blocks[1], external), external);
      await endUsb();
      await memory.fill(lines.join('\n'));
      await decide(true, /^0\/2 blocks written and verified\..*Read the tag again before writing\.$/);
      await expect(write).toHaveText('Write changes');
      await expect(write).toBeDisabled();
      const externallyChanged = structuredClone(prepared.blocks);
      externallyChanged[1] = external;
      await checkMemory(externallyChanged);
      await openUsb();
      await command(2030, writePayload(1, external, prepared.blocks[1]), prepared.blocks[1]);
      await endUsb();
      await checkMemory(prepared.blocks);
      writes.ble.push({
        staleCaptureRejected: true,
        noFurtherBlocksWritten: true,
        readBindingInvalidated: true,
        fixtureRestoredThroughUsb: true
      });
    }
    if (trailerChanges) {
      const memory = page.locator('#memory textarea');
      const write = page.locator('#write-changes');
      const originalText = await memory.inputValue();
      const lines = originalText.split('\n');
      lines[securityTrailer] = Buffer.from(changedSecurity[securityTrailer]).toString('hex');
      await memory.fill(lines.join('\n'));
      await expect(write).toHaveText('Write changes (1)');
      const decide = async (accepted) => {
        await write.click();
        const dialog = page.getByRole('dialog', { name: 'Write changes?', exact: true });
        await expect(dialog).toContainText(`Keys and access rights: sector ${securitySector}.`);
        await dialog
          .getByRole('button', { name: accepted ? 'Write changes' : 'Cancel', exact: true })
          .click();
        await expect(page.locator('#status')).toHaveText(
          accepted
            ? '1 block written and verified · keys/access changed. Read the tag again before writing.'
            : 'Writing cancelled'
        );
        await expect(page.locator('#stop')).toBeHidden();
      };
      await decide(false);
      await checkMemory(prepared.blocks);
      writes.ble.push({ trailerCancelled: true, allMemoryPreserved: true });
      await decide(true);
      await expect(write).toBeDisabled();
      await expect(write).toHaveText('Write changes');
      await openUsb();
      for (const [type, oldKey, newKey] of [
        [0x60, keyA, replacementA],
        [0x61, keyB, replacementB]
      ]) {
        await command(2015, [securityBlock, type, 1, ...oldKey], Array(7).fill(0), 6);
        await command(2015, [securityBlock, type, 1, ...newKey], [1, ...newKey]);
      }
      await endUsb();
      await checkMemory(changedSecurity);
      writes.ble.push({
        sector: securitySector,
        newKeysAuthenticated: true,
        oldKeysRejected: true,
        accessBytesVerified: true,
        readBindingInvalidated: true
      });
      if (splitTrailers) {
        await nextFixturePhase(changedSecurity, 'trailer-new-keys');
      }
      const reread = async (blocks) => {
        await page.locator('#read').click();
        await page
          .getByRole('dialog', { name: 'Read fresh memory?', exact: true })
          .getByRole('button', { name: 'Recover', exact: true })
          .click();
        await expect(page.locator('#status')).toHaveText('Read complete', { timeout: 90000 });
        await expect(page.locator('#stop')).toBeHidden();
        const text = await memory.inputValue();
        expect(text.split('\n').map((line) => [...Buffer.from(line.replace(/\s/g, ''), 'hex')])).toEqual(
          blocks
        );
        await expect(write).toHaveText('Write changes (0)');
      };
      await reread(changedSecurity);
      const securityText = await memory.inputValue();
      const protectedChange = structuredClone(changedSecurity);
      protectedChange[securityBlock] = Array.from({ length: 16 }, (_, index) => index ^ 0xc3);
      // This PM3 simulator ACKs denied data writes; it cannot qualify a
      // real tag's negative permission behavior. Trace the positive B path.
      const protectedLines = securityText.split('\n');
      protectedLines[securityBlock] = Buffer.from(protectedChange[securityBlock]).toString('hex');
      const writeProtected = async (text) => {
        await memory.fill(text);
        await expect(write).toHaveText('Write changes (1)');
        await write.click();
        const dialog = page.getByRole('dialog', { name: 'Write changes?', exact: true });
        await expect(dialog).toContainText(`Tag 11223344 · 1 data block: ${securityBlock}.`);
        await dialog.getByRole('button', { name: 'Write changes', exact: true }).click();
        await expect(page.locator('#status')).toHaveText('1 block written and verified');
        await expect(page.locator('#stop')).toBeHidden();
        await expect(write).toHaveText('Write changes (0)');
      };
      await writeProtected(protectedLines.join('\n'));
      await checkMemory(protectedChange);
      await writeProtected(securityText);
      await checkMemory(changedSecurity);
      writes.ble.push({
        block: securityBlock,
        accessCondition: '100',
        realTagKeyAWriteRefusalQualified: false,
        keyBOnlyWriteAndRestoreThroughUi: true,
        allMemoryIndependentlyVerified: true
      });
      if (splitTrailers) {
        await nextFixturePhase(changedSecurity, 'trailer-restore-keys');
      }
      await memory.fill(originalText);
      await expect(write).toHaveText('Write changes (1)');
      await decide(true);
      await openUsb();
      for (const [type, oldKey, newKey] of [
        [0x60, keyA, replacementA],
        [0x61, keyB, replacementB]
      ]) {
        await command(2015, [securityBlock, type, 1, ...newKey], Array(7).fill(0), 6);
        await command(2015, [securityBlock, type, 1, ...oldKey], [1, ...oldKey]);
      }
      await endUsb();
      await checkMemory(prepared.blocks);
      writes.ble.push({
        sector: securitySector,
        originalKeysAndAccessRestoredThroughUi: true,
        replacementKeysRejected: true,
        allMemoryPreserved: true
      });
      await reread(prepared.blocks);
    }
    expect(leakedHttp).toEqual([]);
    checkChooser();
    qualified = true;
  } finally {
    try {
      if (unknownKey) {
        const progress = await page.evaluate(() => globalThis.classicRecoveryProgress || []).catch(() => []);
        await writeFile(testInfo.outputPath('recovery-progress.json'), JSON.stringify(progress, null, 2));
      }
      try {
        if (qualified) {
          // The UI refreshes device/slot metadata after Read. Observe its final
          // replies before closing the page; never replay a device request.
          await expect
            .poll(
              async () => {
                gattTrace = await saveGattTrace(testInfo.outputPath('gatt-trace.json'));
                return gattTrace.pending;
              },
              { timeout: 3000 }
            )
            .toEqual([]);
        } else {
          gattTrace = await saveGattTrace(testInfo.outputPath('gatt-trace.json')).catch(async (error) => {
            await writeFile(testInfo.outputPath('gatt-trace-error.txt'), error.stack || error.message);
          });
        }
      } finally {
        await page.close();
        if (session) {
          expect(
            (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
          ).toBe(200);
        }
      }
    } finally {
      try {
        if (stopSimulation) {
          await stopSimulation();
        }
      } finally {
        const current = await saveRam('pm3-after');
        if (!current.equals(originalRam)) {
          await pm3(`hf mf eload --4k -f ${JSON.stringify(restoreBin)}`, 'pm3-fallback-restore');
          expect(await saveRam('pm3-fallback-verified')).toEqual(originalRam);
          throw new Error('PM3 needed fallback RAM restoration; autonomous cleanup did not qualify');
        }
      }
    }
    if (unknownKey) {
      await pm3('trace list -t 14a -r -u', 'pm3-rf-trace');
    }
    cleanup = JSON.parse(await readFile(`${simulationLog}.cleanup.json`, 'utf8'));
    expect(cleanup).toMatchObject({
      stopped: true,
      persistentStopPingConfirmed: true,
      ramRestored: true,
      ramSha256: hash(originalRam)
    });
    await expect.poll(async () => (await info()).busy).toBe(false);
    const lease = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
    expect(lease.status()).toBe(200);
    const negativeSession = (await lease.json()).session;
    try {
      const absent = await request.post(`${deviceURL}/api/command/2000?id=${negativeSession}`, {
        data: Buffer.alloc(0)
      });
      expect(absent.status()).toBe(200);
      expect([...(await absent.body())]).toEqual([0, 1]);
    } finally {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${negativeSession}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
    }
    const after = await info();
    for (const key of ['chip', 'git', 'settings', 'reader', 'active_slot']) {
      expect(after[key], key).toEqual(before[key]);
    }
    expect(await usbSlotSnapshot(request, deviceURL)).toEqual(slotsBefore);
    if (qualified) {
      expect(gattTrace?.errors).toEqual([]);
      expect(gattTrace?.truncated).toBe(false);
      expect(gattTrace?.pending).toEqual([]);
      if (!writeChanges && !trailerChanges && !copyData && !formatCard) {
        expect(
          gattTrace.requests.filter(
            (entry) => entry.operation === 8 && [2030, 2033, 2034, 2038, 2039].includes(entry.argument)
          )
        ).toEqual([]);
      }
      if (size === '2k') {
        const inspections = gattTrace.requests.filter(
          (entry) => entry.operation === 8 && entry.argument === 2040
        );
        if (formatCard) {
          // Formatting invalidates the previous observation; checking the
          // already-formatted card must inspect it again.
          expect(inspections.length).toBeGreaterThanOrEqual(2);
        } else {
          expect(inspections).toHaveLength(copyData ? 4 : 1);
        }
        for (const inspection of inspections) {
          expect(inspection.rfStatus).toBe(0);
          expect(inspection.reply.slice(13)).toEqual([1, 14, ...model.ats, 0x60, 0xd3]);
        }
      }
      if (copyData) {
        const mutations = gattTrace.requests.filter(
          (entry) => entry.operation === 8 && [2030, 2033, 2034, 2038, 2039].includes(entry.argument)
        );
        expect(
          mutations.map((entry) => [entry.argument, entry.body[0], entry.body[9], entry.rfStatus])
        ).toEqual([
          [2030, 6, 1, 0],
          [2030, 6, 126, 0]
        ]);
      }
      if (unknownKey) {
        expect(
          gattTrace.requests.some(
            (entry) => entry.operation === 8 && entry.argument === 2023 && entry.rfStatus === 0
          )
        ).toBe(true);
        expect(
          gattTrace.requests.some(
            (entry) =>
              entry.operation === 8 &&
              entry.argument === 2015 &&
              entry.body[0] === 4 &&
              entry.body[1] === 0x60 &&
              String(entry.reply) === String([0, 0, 1, ...recoveryKey])
          )
        ).toBe(true);
        expect(
          gattTrace.requests.filter(
            (entry) => entry.operation === 8 && [2030, 2033, 2034, 2038, 2039].includes(entry.argument)
          )
        ).toEqual([]);
      }
      if (trailerChanges) {
        const trailers = gattTrace.requests.filter(
          (entry) => entry.operation === 8 && entry.argument === 2034
        );
        expect(trailers.map((entry) => [entry.body.length, entry.body[10], entry.rfStatus])).toEqual([
          [38, 1, 0x75],
          [38, 2, 0],
          [37, 2, 0],
          [38, 1, 0x75],
          [38, 2, 0],
          [37, 2, 0]
        ]);
        const dataWrites = gattTrace.requests.filter(
          (entry) => entry.operation === 8 && entry.argument === 2030
        );
        expect(dataWrites.map((entry) => [entry.body[9], entry.body[10], entry.rfStatus])).toEqual([
          [securityBlock, 2, 0],
          [securityBlock, 2, 0]
        ]);
      }
      if (formatCard) {
        const maintenance = gattTrace.requests.filter(
          (entry) => entry.operation === 8 && entry.argument === 2039
        );
        for (const entry of maintenance) {
          expect([20, 52]).toContain(entry.body.length);
          expect(entry.body.slice(1, 8)).toEqual([3, 6, 4, ...uid]);
          expect(entry.body.at(-1)).toBe(1);
        }
        const trailers = sectors.map((sector) => sector.trailer);
        const data = prepared.blocks
          .map((_, index) => index)
          .filter((index) => index && !trailers.includes(index));
        const mutations = maintenance.filter((entry) => entry.body[0] === 2);
        expect(mutations.map((entry) => entry.body[11])).toEqual([...data, ...trailers]);
        for (const entry of mutations) {
          const index = entry.body[11];
          const trailer = trailers.includes(index);
          expect(entry.body.length).toBe(52);
          expect(entry.body[12]).toBe(trailer ? 2 : 1);
          expect(entry.body.slice(35, 51)).toEqual(
            trailer ? factoryTrailer(index === securityTrailer) : Array(16).fill(0)
          );
          expect(entry.rfStatus).toBe(0);
        }
        expect(
          gattTrace.requests.filter(
            (entry) => entry.operation === 8 && [2030, 2033, 2034, 2038].includes(entry.argument)
          )
        ).toEqual([]);
        expect(fixturePhaseCleanups).toHaveLength(1);
      }
    }
    await testInfo.attach('physical-reader-classic-evidence', {
      body: JSON.stringify(
        {
          qualified,
          preserved: true,
          chip: before.chip,
          firmware: before.git,
          usb,
          card: captured?.card,
          classicSize: size,
          classicStopReport,
          blocksCompared: qualified ? model.blocks : 0,
          sectorsWithBothKeys: qualified ? sectors.length : 0,
          knownDictionaryKeysOnly: !unknownKey,
          unknownKeyAttacksQualified: false,
          unknownKeyRecovery: unknownKey
            ? {
                qualified,
                sector: 1,
                key: 'A',
                absentFromProductionDictionaries: true,
                origin: captured?.recoveredKeys?.[1]?.origin_a,
                scope: 'One synthetic PM3 Classic 1K key; not general attack qualification'
              }
            : null,
          pm3Cleanup: cleanup,
          copyCancellationCleanup,
          trailerPhaseCleanups: trailerChanges ? fixturePhaseCleanups : [],
          formatPhaseCleanups: formatCard ? fixturePhaseCleanups : [],
          pm3RamSha256: hash(originalRam),
          simulationAbsentAfterStop: true,
          httpFallbackRequests: leakedHttp,
          physicalTagWrites: false,
          writesToPm3EmulatorRam: writes,
          gattRequestsRecorded: gattTrace?.requests.length,
          gattFrameErrors: gattTrace?.errors,
          fixture: unknownKey
            ? 'PM3 Classic 1K volatile RAM; one non-dictionary sector-1 key A, synthetic memory'
            : `PM3 Classic ${size} volatile emulator RAM; distinct built-in keys A/B, synthetic memory`
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
}

test('the real BLE UI recovers known Classic 1K keys and reads all memory identically to USB', ({
  page,
  request
}, info) => classicBench({ page, request }, info));

test('a real Classic 1K rejects all model-6 mutation paths before the BLE reference read', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { rejectTwoK: true }));

test('the real BLE UI cancels, writes, verifies and restores Classic 1K simulated data blocks', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { writeChanges: true }));

test('the real BLE UI cancels, changes and restores Classic keys and access through key B', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { trailerChanges: true }));

test('the real BLE UI recovers a non-dictionary Classic key from physical nonce acquisition', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { unknownKey: true }));

test('the real BLE UI writes, rejects stale data and restores upper Plus SL1 2K blocks', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { size: '2k', writeChanges: true }));

test('the real BLE UI changes and restores Plus SL1 2K sector-31 keys and access', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { size: '2k', trailerChanges: true }));

test('the real BLE UI cancels and copies upper Plus SL1 2K data with fresh capacity checks', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { size: '2k', copyData: true }));

test('the real BLE UI cancels, formats all Plus SL1 2K sectors and skips an already-formatted card', ({
  page,
  request
}, info) => classicBench({ page, request }, info, { size: '2k', formatCard: true }));

for (const [size, label] of [
  ['mini', 'Mini'],
  ['2k', '2K'],
  ['4k', '4K']
]) {
  test(`the real BLE UI reads all Classic ${label} sectors identically to USB after stop qualification`, ({
    page,
    request
  }, info) => classicBench({ page, request }, info, { size }));
}
