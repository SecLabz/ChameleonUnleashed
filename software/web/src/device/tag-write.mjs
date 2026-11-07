import { type2Changes, type2UserEnd } from '../documents/type2-content.mjs';
import { withRuntime } from '../runtime/runtime.mjs';
import {
  magicGen1aDetected,
  magicGen2Confirmed,
  magicCloneSupported,
  classicObservationMatches,
  GEN2_INCONCLUSIVE
} from '../documents/classic-magic.mjs';
import { classicChanges, classicWriteGroups } from '../documents/classic-write.mjs';
import { bytesValid, family } from '../documents/slot-image.mjs';
import { scanNfcA } from './hf-reader.mjs';
import { COMMAND } from './commands.mjs';
import { withDevice } from './transport.mjs';
import { Device, hex } from './device.mjs';
import { inspectClassic } from './classic-inspect.mjs';
import { filterDictionary } from '../recovery/dictionary.mjs';

const MODELS = {
  1000: 5,
  1001: 1,
  1002: 6,
  1003: 7,
  1100: 8,
  1101: 2,
  1102: 9,
  1104: 19,
  1105: 11,
  1106: 12,
  1107: 20,
  1108: 21
};
const VERSIONED = new Set([2, 8, 9, 11, 12, 20, 21]);

export function supportsPhysicalCopy(source) {
  if (!source?.memory?.length) {
    return false;
  }

  if (source.kind === 'lf') {
    return family(source.type) === 'lf';
  }

  const model = MODELS[source.type];
  if (source.kind === 'classic') {
    return [1, 5, 6, 7].includes(model);
  }
  return source.kind === 'type2' && (model === 19 || VERSIONED.has(model));
}

const bytesEqual = (left, right) =>
  !!left && !!right && left.length === right.length && left.every((value, index) => value === right[index]);

async function classicCopyCapacity(device, source, card, onProgress) {
  const sak = card.sak & ~0x20;
  let blocks = { 9: 20, 8: 64, 16: 128, 24: 256 }[sak];
  let ats = card.ats || [];
  if (sak === 8) {
    // SAK 08 is shared by 1K and Plus SL1 2K. A source image or cached scan
    // cannot establish the capacity of a different physical target.
    const observation = await inspectClassic(device, card, onProgress, { modelEvidence: true });
    if (!observation.modelEvidence) {
      throw new Error('Update the firmware to verify the target Classic capacity before copying.');
    }
    ats = observation.ats || [];
    if (ats.length && observation.type !== 1002) {
      throw new Error('The target Classic capacity cannot be established from its ATS.');
    }
    blocks = observation.type === 1002 ? 128 : 64;
  }
  if (source.geometry.blocks !== blocks) {
    throw new Error('The target Classic size differs');
  }
  return ats;
}

function isUserUnit(document, index) {
  if (document.kind === 'classic') {
    return index > 0 && !document.geometry.sectors.some((sector) => sector.trailer === index);
  }

  return index >= 4 && index < type2UserEnd(document);
}

async function classicTarget(device, document, card, call, dictionary, onProgress, knownKeys = []) {
  const geometry = document.geometry;
  const memory = Array(geometry.blocks).fill(null);
  const sectorKeys = [];

  const { nonce_keys: builtins } = await call({
    op: 'dictionary',
    text: dictionary,
    anchor: null,
    profile: 'Extended'
  });
  const knownKey = (bytes) =>
    bytes?.length === 6 && bytes.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 255);
  const keys = new Map(
    knownKeys
      .flat()
      .filter(knownKey)
      .map((key) => [hex(key), key])
  );
  let anchor = null;

  for (const [sector, group] of geometry.sectors.entries()) {
    const sourceTrailer = document.memory[group.trailer];
    const sourceA = sourceTrailer?.slice(0, 6);
    const sourceB = sourceTrailer?.slice(10, 16);
    const findKey = async (keyB) => {
      const preferred = new Map(
        [...(knownKeys[sector] || []), ...keys.values(), sourceA, sourceB]
          .filter(knownKey)
          .map((key) => [hex(key), key])
      );
      const remaining = builtins.filter((key) => !preferred.has(hex(key)));
      const candidates = [...preferred.values(), ...remaining];
      let tested = 0;
      const progress = () =>
        onProgress(
          `Checking target sector ${sector + 1} / ${geometry.sectors.length} · key ${keyB ? 'B' : 'A'} · ${tested} / ${candidates.length} keys tested…`
        );
      progress();
      const onTested = (batch) => {
        tested += batch.length;
        progress();
      };
      const direct = anchor ? [...preferred.values()] : candidates;
      const key = await device.findKey(sector, keyB, direct, geometry, onTested);
      if (key || direct.length === candidates.length) {
        return key;
      }

      try {
        const result = await filterDictionary({
          call,
          uid: card.uid,
          keys: remaining,
          collect: (count) => {
            onProgress(
              `Checking target sector ${sector + 1} / ${geometry.sectors.length} · filtering key ${keyB ? 'B' : 'A'} candidates…`
            );
            return device.collectNonces(anchor, { sector, is_key_b: keyB }, count, geometry);
          }
        });
        const filtered = await device.findKey(sector, keyB, result.candidates, geometry, onTested);
        if (filtered || result.exhausted) {
          return filtered;
        }
      } catch (error) {
        if (![6, 0x67, 0x69].includes(error.status) && error.httpStatus !== 404) {
          throw error;
        }
      }
      return device.findKey(sector, keyB, remaining, geometry, onTested);
    };

    const keyA = await findKey(false);
    if (keyA) {
      anchor = { sector, is_key_b: false, key: keyA };
      keys.set(hex(keyA), keyA);
    }
    let visibleB = null;
    if (keyA) {
      try {
        const trailer = await device.readBlock(group.trailer, keyA, false);
        visibleB = (await call({ op: 'trailer_key_b', block: trailer })).key;
      } catch (error) {
        if (![2, 6, 7].includes(error.status)) {
          throw error;
        }
      }
    }
    const keyB = visibleB ? null : await findKey(true);
    if (keyB && !anchor) {
      anchor = { sector, is_key_b: true, key: keyB };
    }
    sectorKeys[sector] = [keyA, keyB];
    for (const key of [keyB, visibleB]) {
      if (key) {
        keys.set(hex(key), key);
      }
    }
    for (let block = group.first; block < group.first + group.count; block++) {
      for (const [key, kind] of [
        [keyA, 1],
        [keyB, 2]
      ]) {
        if (!key) {
          continue;
        }
        try {
          memory[block] = await device.readBlock(block, key, kind === 2);
          break;
        } catch (error) {
          if (![2, 6, 7].includes(error.status)) {
            throw error;
          }
        }
      }
    }
  }

  const identityMemory = memory.slice();

  for (const [sector, group] of geometry.sectors.entries()) {
    const raw = memory[group.trailer];
    if (!raw) {
      continue;
    }
    const [keyA, keyB] = sectorKeys[sector];
    const readableB = (await call({ op: 'trailer_key_b', block: raw })).key;
    identityMemory[group.trailer] =
      keyA && (keyB || readableB) ? [...keyA, ...raw.slice(6, 10), ...(keyB || readableB)] : null;
  }

  const credentials = await classicWriteCredentials(document, memory, sectorKeys, call);
  return { memory, identityMemory, credentials, sectorKeys, writable: credentials.map(Boolean) };
}

async function classicWriteCredentials(document, memory, sectorKeys, call) {
  const credentials = Array(memory.length).fill(null);
  // Permissions depend on access bytes, not hidden key bytes in partial trailers.
  const accessMemory = memory.map((block) =>
    block?.length === 16 && bytesValid(block.slice(6, 10)) ? block.map((byte) => byte ?? 0) : null
  );
  const permissionsA = await call({
    op: 'physical_permissions',
    model: MODELS[document.type],
    memory: accessMemory,
    key_b: [],
    authenticated: false
  });
  const permissionsB = await call({
    op: 'physical_permissions',
    model: MODELS[document.type],
    memory: accessMemory,
    key_b: Array(memory.length).fill(true),
    authenticated: false
  });

  for (const [sector, group] of document.geometry.sectors.entries()) {
    const trailer = memory[group.trailer];
    if (!trailer) {
      continue;
    }
    const [keyA, keyB] = sectorKeys[sector] || [];
    for (let block = group.first; block < group.trailer; block++) {
      if (keyA && permissionsA.writable[block]) {
        credentials[block] = { kind: 1, key: keyA };
      } else if (keyB && permissionsB.writable[block]) {
        credentials[block] = { kind: 2, key: keyB };
      }
    }
  }

  return credentials;
}

async function type2Target(device, document, call, credential = null, changedPages = null) {
  const model = MODELS[document.type];
  const ultralightC = model === 19;
  if (!ultralightC && !VERSIONED.has(model)) {
    throw new Error('This Type 2 model cannot be verified for physical writing');
  }

  const request = (op, page = 0, endPage) =>
    device.command(COMMAND.TYPE2_READ, [
      op,
      page,
      device.card.uid.length,
      ...device.card.uid,
      ...(endPage === undefined ? [] : [endPage])
    ]);
  if (!ultralightC) {
    const version = Array.from(await request(0));
    const detected = await call({ op: 'type2_geometry', version });
    if (detected.profile !== document.geometry.profile) {
      throw new Error('The target Type 2 model differs');
    }
  }

  const memory = Array(document.geometry.pages).fill(null);
  const readablePages = ultralightC ? 44 : memory.length;
  let pages = Array.from({ length: readablePages }, (_, page) => page);
  if (changedPages) {
    // Permissions depend on static locks and the public lock/configuration tail,
    // not on unchanged user data. Never trust cached security bytes for a write.
    const end = Math.min(document.geometry.credentials?.[0] ?? readablePages, readablePages);
    const start = type2UserEnd(document);
    const security = Array.from({ length: end - start }, (_, offset) => start + offset);
    pages = [...new Set([2, ...changedPages, ...security])].sort((left, right) => left - right);
  }
  let fast =
    !!changedPages && !credential && ['ntag213', 'ntag215', 'ntag216'].includes(document.geometry.profile);
  for (let index = 0; index < pages.length; ) {
    const page = pages[index];
    let count = 1;
    while (fast && count < 8 && pages[index + count] === page + count) {
      count++;
    }
    try {
      const bytes =
        credential || ultralightC
          ? await device.command(COMMAND.TYPE2_AUTH_READ, [
              model,
              7,
              ...device.card.uid,
              page,
              credential?.kind || 0,
              ...(credential?.key || []),
              ...Array(16 - (credential?.key.length || 0)).fill(0)
            ])
          : await request(fast ? 5 : 1, page, fast ? page + count - 1 : undefined);
      if (bytes.length !== (fast ? count * 4 : 16)) {
        throw new Error('Invalid page read');
      }
      // READ can wrap at AUTH0 or the last readable page. Keep its first page only.
      for (let offset = 0; offset < count; offset++) {
        memory[page + offset] = Array.from(bytes.slice(offset * 4, offset * 4 + 4));
      }
    } catch (error) {
      if (fast && [2, 3, 7, 0x60, 0x69].includes(error.status)) {
        // Unsupported FAST_READ or an access boundary: retry only needed pages
        // with READ, whose first page is reliable even when its reply wraps.
        fast = false;
        continue;
      }
      if (ultralightC && [0x60, 0x67, 0x69].includes(error.status)) {
        throw new Error('Update the firmware for Ultralight C physical restoration');
      }
      if ((ultralightC && page === 0) || ![2, 3, 7].includes(error.status)) {
        throw error;
      }
    }
    index += count;
  }

  const { writable } = await call({
    op: 'physical_permissions',
    model,
    memory,
    key_b: [],
    authenticated: !!credential
  });

  return {
    memory,
    identityMemory: memory,
    writable,
    credentials: credential ? Array(memory.length).fill(credential) : []
  };
}

function writePayload(model, uid, unit, credential) {
  const { before, after } = unit;
  const key = credential?.key || [];
  return [
    model,
    uid.length,
    ...uid,
    ...Array(7 - uid.length).fill(0),
    unit.index,
    credential?.kind || 0,
    ...key,
    ...Array(16 - key.length).fill(0),
    ...before,
    ...Array(16 - before.length).fill(0),
    ...after,
    ...Array(16 - after.length).fill(0)
  ];
}

async function writeUnits(device, model, uid, units, credentials, onProgress, onVerified = () => {}) {
  for (const unit of units) {
    device.check();
    onProgress(`Writing unit ${unit.index}…`);
    const reply = await device.writeCommand(
      COMMAND.WRITE_USER_UNIT,
      writePayload(model, uid, unit, credentials[unit.index])
    );
    if (!bytesEqual(reply, unit.after)) {
      throw new Error(`Unit ${unit.index}: verification mismatch. Inspect the tag before retrying.`);
    }
    onVerified(unit);
  }
}

function trailerPayload(model, uid, unit, credential) {
  return [
    uid.length,
    ...uid,
    ...Array(7 - uid.length).fill(0),
    model,
    unit.sector,
    credential.kind,
    ...credential.key,
    ...unit.before.slice(6, 10),
    ...unit.after.slice(0, 6),
    ...unit.after.slice(10, 16),
    ...unit.after.slice(6, 10)
  ];
}

// Let the firmware check trailer permissions; it also owns their write verification.
async function checkTrailers(device, model, uid, units, sectorKeys) {
  const credentials = [];
  for (const unit of units) {
    device.check();
    const keys = sectorKeys[unit.sector] || [];
    for (const [index, key] of keys.entries()) {
      if (!key) {
        continue;
      }
      const credential = { kind: index + 1, key };
      try {
        const reply = await device.command(COMMAND.SET_TRAILER, [
          ...trailerPayload(model, uid, unit, credential),
          1
        ]);
        if (reply.length) {
          throw new Error('Invalid trailer check reply');
        }
        credentials[unit.index] = credential;
        break;
      } catch (error) {
        // This read-only preflight can reject A's permissions with CMD_ERR.
        // Try the other known key; no writes run until every trailer passes.
        if (![2, 6, 7, 0x70, 0x75].includes(error.status)) {
          const reason =
            error.status === 0x60
              ? 'Invalid trailer access bytes, or firmware needs updating'
              : error.message;
          throw new Error(`Sector ${unit.sector}: ${reason}. Update the firmware if needed.`);
        }
      }
    }
    if (!credentials[unit.index]) {
      throw new Error(
        `Sector ${unit.sector}: no known key permits writing both keys and the requested access rights`
      );
    }
  }
  return credentials;
}

async function writeTrailers(device, model, uid, units, credentials, onProgress, onVerified) {
  for (const unit of units) {
    device.check();
    onProgress(`Writing keys and access rights for sector ${unit.sector}…`);
    const reply = await device.writeCommand(
      COMMAND.SET_TRAILER,
      trailerPayload(model, uid, unit, credentials[unit.index])
    );
    if (reply.length) {
      throw new Error(
        `Sector ${unit.sector}: invalid trailer verification reply. Inspect the tag before retrying.`
      );
    }
    onVerified(unit);
  }
}

export async function writeType2Changes(source, signal, onProgress, { confirmChanges, requestCredential }) {
  const changes = type2Changes(source);
  if (!changes || changes.unknown.length) {
    throw new Error('Read the changed Type 2 pages in this session before writing.');
  }
  if (!changes.writes.length) {
    onProgress('No user-page changes to write');
    return false;
  }
  const reading = source.type2Read;
  const device = new Device(signal);
  let completed = 0;
  try {
    return await withRuntime(
      (call) =>
        withDevice(device, async () => {
          const card = await device.begin();
          if (!bytesEqual(card.uid, reading.card.uid) || card.sak !== reading.card.sak) {
            throw new Error('The physical tag differs from the last read. No writes were sent.');
          }
          onProgress('Checking changed pages and locks…');
          const pages = changes.writes.map((unit) => unit.index);
          let target = await type2Target(device, source, call, null, pages);
          if (changes.writes.some((unit) => !target.writable[unit.index])) {
            const key = await requestCredential(source.type === 1104, signal);
            if (key) {
              target = await type2Target(
                device,
                source,
                call,
                { kind: source.type === 1104 ? 4 : 3, key },
                pages
              );
            }
          }
          for (const unit of changes.writes) {
            if (!target.writable[unit.index]) {
              throw new Error(`Page ${unit.index} is locked or its write permissions are unknown.`);
            }
            if (!bytesEqual(target.memory[unit.index], unit.before)) {
              throw new Error(`Page ${unit.index} changed since reading. No writes were sent.`);
            }
          }
          if (!(await confirmChanges({ uid: card.uid, ...changes }, signal))) {
            onProgress('Writing cancelled');
            return;
          }
          await writeUnits(
            device,
            MODELS[source.type],
            card.uid,
            changes.writes,
            target.credentials,
            onProgress,
            (unit) => {
              reading.memory[unit.index] = [...unit.after];
              completed++;
            }
          );
          onProgress(`${completed} ${completed === 1 ? 'page' : 'pages'} written and verified`);
          return true;
        }),
      signal
    );
  } catch (error) {
    delete source.type2Read;
    throw new Error(
      `${completed}/${changes.writes.length} pages written and verified. ${error.message} Read the tag again before writing.`
    );
  }
}

export async function writeClassicChanges(source, signal, onProgress, { confirmChanges }) {
  const changes = classicChanges(source);
  if (!changes) {
    throw new Error('Read this Classic tag in the current session before writing changes');
  }
  if (changes.unknown.length) {
    throw new Error(`Blocks ${changes.unknown.join(', ')} need complete original and edited bytes`);
  }
  if (!changes.writes.length) {
    onProgress('No changes to write');
    return;
  }

  const reading = source.classicRead;
  const { data, trailers } = classicWriteGroups(source, changes.writes);
  const sectorKeys = reading.keys.map((keys) => [
    keys.key_a,
    keys.origin_b === 'Trailer' ? null : keys.key_b
  ]);
  const credentials = await withRuntime(
    (call) => classicWriteCredentials(source, reading.memory, sectorKeys, call),
    signal
  );
  const protectedBlocks = data.filter((unit) => !credentials[unit.index]);
  if (protectedBlocks.length) {
    throw new Error(`No known write key for blocks ${protectedBlocks.map((unit) => unit.index).join(', ')}`);
  }

  if (!(await confirmChanges({ uid: reading.card.uid, ...changes, data, trailers }, signal))) {
    onProgress('Writing cancelled');
    return;
  }

  const device = new Device(signal);
  let completed = 0;
  try {
    await withDevice(device, async () => {
      onProgress('Present the tag read in this session…');
      const card = await device.begin();
      if (
        !bytesEqual(card.uid, reading.card.uid) ||
        card.sak !== reading.card.sak ||
        !bytesEqual(card.atqa, reading.card.atqa)
      ) {
        throw new Error('The physical tag differs from the last read. No writes were sent.');
      }

      onProgress('Checking keys and access rights…');
      const trailerCredentials = await checkTrailers(
        device,
        MODELS[source.type],
        card.uid,
        trailers,
        sectorKeys
      );
      const verified = (unit) => {
        reading.memory[unit.index] = [...unit.after];
        completed++;
      };
      await writeUnits(
        device,
        MODELS[source.type],
        reading.card.uid,
        data,
        credentials,
        onProgress,
        verified
      );
      // All data is written with the original permissions before any keys change.
      await writeTrailers(
        device,
        MODELS[source.type],
        card.uid,
        trailers,
        trailerCredentials,
        onProgress,
        verified
      );
    });
  } catch (error) {
    delete source.classicRead;
    if (trailers.length) {
      delete source.recoveredKeys;
    }
    throw new Error(
      `${completed}/${changes.writes.length} blocks written and verified. ${error.message} Read the tag again before writing.`
    );
  }
  if (completed) {
    let message = `${completed} block${completed === 1 ? '' : 's'} written and verified`;
    if (trailers.length) {
      delete source.classicRead;
      delete source.recoveredKeys;
      message += ' · keys/access changed. Read the tag again before writing.';
    }
    onProgress(message);
  }
  return completed;
}

async function checkClassicClone(device, source, card, target, plan, activationMatches) {
  const unavailable = (reason) => ({ reason });
  if (source.card.uid.length !== 4 || card.uid.length !== 4) {
    return unavailable('Cloning currently requires four-byte Classic UIDs.');
  }
  if (!source.memory.every((block) => block?.length === 16 && bytesValid(block))) {
    return unavailable('Read or supply every source block, including both keys in each sector.');
  }
  const block0 = source.memory[0];
  if (
    !bytesEqual(block0.slice(0, 4), source.card.uid) ||
    (block0[0] ^ block0[1] ^ block0[2] ^ block0[3]) !== block0[4]
  ) {
    return unavailable('The source UID and block 0 must agree, with a valid UID check byte.');
  }
  if (!activationMatches) {
    return unavailable('Target ATQA, SAK or ATS differs. This clone path cannot configure them separately.');
  }
  if (plan.skipped.length || target.identityMemory.some((block) => !block || !bytesValid(block))) {
    return unavailable('Some target blocks are protected or could not be checked.');
  }
  const trailers = source.geometry.sectors
    .map((group, sector) => ({
      sector,
      index: group.trailer,
      before: target.identityMemory[group.trailer],
      after: source.memory[group.trailer]
    }))
    .filter((unit) => !bytesEqual(unit.before, unit.after));
  const identity = bytesEqual(block0, target.memory[0])
    ? null
    : { index: 0, before: target.memory[0], after: block0 };
  let credential = null;
  try {
    if (identity) {
      let magic = await device.probeMagic(card);
      if (!magicGen1aDetected(magic)) {
        for (const [index, key] of (target.sectorKeys[0] || []).entries()) {
          if (!key) {
            continue;
          }
          magic = await device.probeMagic(card, key, index === 1);
          if (magicGen2Confirmed(magic)) {
            credential = { kind: index + 1, key };
            break;
          }
        }
      }
      if (!magicCloneSupported(magic) || (!magicGen1aDetected(magic) && !credential)) {
        return unavailable(
          'Block 0 requires a supported Gen1A or CUID target. Gen4 and one-time tags are not supported.'
        );
      }
      const reply = await device.command(COMMAND.SET_BLOCK0, [
        ...writePayload(MODELS[source.type], card.uid, identity, credential),
        1
      ]);
      if (!bytesEqual(reply, identity.before)) {
        return unavailable('The target block 0 changed during inspection. Check the target again.');
      }
    }
    const credentials = await checkTrailers(
      device,
      MODELS[source.type],
      card.uid,
      trailers,
      target.sectorKeys
    );
    return {
      trailers,
      credentials,
      identity,
      credential,
      writes: plan.writes.length + trailers.length + Number(!!identity)
    };
  } catch (error) {
    device.check();
    return unavailable(`Clone check failed: ${error.message}`);
  }
}

export async function copyToTag(
  source,
  signal,
  onProgress,
  { requestCredential, confirmCopy, dictionary = '' }
) {
  const model = MODELS[source?.type];
  if (!supportsPhysicalCopy(source)) {
    throw new Error('Open a supported Classic or Type 2 memory image first');
  }

  const device = new Device(signal);
  let completed = 0;
  let total = 0;
  try {
    return await withDevice(device, async () => {
      onProgress('Present the target tag…');
      const card = await device.begin();
      if ((source.kind === 'classic') !== [8, 9, 16, 24].includes(card.sak & ~0x20)) {
        throw new Error('The target tag family differs');
      }
      if (source.kind === 'classic') {
        card.ats = await classicCopyCapacity(device, source, card, onProgress);
      }
      if (source.kind === 'type2' && card.uid.length !== 7) {
        throw new Error('Expected a seven-byte Type 2 UID');
      }

      onProgress('Checking target memory and write access…');
      let credential = null;
      const result = await withRuntime(async (call) => {
        let target =
          source.kind === 'classic'
            ? await classicTarget(device, source, card, call, dictionary, onProgress)
            : await type2Target(device, source, call);

        let needsCredential = false;
        if (source.kind === 'type2') {
          const protectedFrom =
            model === 19
              ? (target.memory[42]?.[0] ?? 0)
              : (target.memory[source.geometry.pages - 4]?.[3] ?? 0);
          needsCredential = target.memory.some((bytes, index) => {
            if (!isUserUnit(source, index)) {
              return false;
            }

            const protectedDifference =
              !target.writable[index] && index >= protectedFrom && !bytesEqual(bytes, source.memory[index]);
            return bytes === null || protectedDifference;
          });
        }
        if (needsCredential) {
          const key = await requestCredential(model === 19, signal);
          if (key) {
            if (
              key.length !== (model === 19 ? 16 : 4) ||
              !key.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 255)
            ) {
              throw new Error('Invalid target credential');
            }
            credential = { kind: model === 19 ? 4 : 3, key };
            target = await type2Target(device, source, call, credential);
          }
        }

        const units = source.memory
          .map((after, index) => ({
            index,
            before: target.memory[index],
            after: after?.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 255) ? after : null,
            writable: !!target.writable[index]
          }))
          .filter((unit) => isUserUnit(source, unit.index));
        const plan = await call({ op: 'physical_plan', model, units, best_effort: true });
        const excluded = source.memory
          .map((bytes, index) =>
            !isUserUnit(source, index) && !bytesEqual(bytes, target.identityMemory[index]) ? index : null
          )
          .filter((index) => index !== null);

        const activationMatches =
          bytesEqual(source.card.atqa, card.atqa) &&
          source.card.sak === card.sak &&
          bytesEqual(source.card.ats || [], card.ats || []);
        return { target, plan, excluded, activationMatches };
      }, signal);

      const { target, plan, excluded, activationMatches } = result;
      const unitLabel = source.kind === 'classic' ? 'block' : 'page';
      const omissions = [
        !bytesEqual(source.card.uid, card.uid) ? 'UID' : null,
        !activationMatches ? 'activation data' : null,
        excluded.length ? `${excluded.length} identity, security or configuration ${unitLabel}s` : null,
        plan.skipped.length ? `${plan.skipped.length} unreadable or protected data ${unitLabel}s` : null,
        source.kind === 'type2' ? 'non-readable Type 2 metadata' : null
      ].filter(Boolean);

      const clone =
        source.kind === 'classic'
          ? await checkClassicClone(device, source, card, target, plan, activationMatches)
          : null;
      const selection = await confirmCopy(
        {
          sourceUid: source.card.uid,
          targetUid: card.uid,
          unitLabel,
          writes: plan.writes.length,
          unchanged: plan.unchanged,
          omissions,
          clone
        },
        signal
      );
      if (!selection) {
        onProgress('Copy cancelled', { phase: 'cancelled' });
        return;
      }
      const cloning = selection === 'clone';
      if ((selection !== 'data' && !cloning) || (cloning && (!clone || clone.reason))) {
        throw new Error('This copy mode is unavailable. Check the target again.');
      }
      total = cloning ? clone.writes : plan.writes.length;

      onProgress('Checking that the target has not changed…');
      if (source.kind === 'classic') {
        const ats = await classicCopyCapacity(device, source, card, onProgress);
        if (!bytesEqual(card.ats, ats)) {
          throw new Error('The target changed during review. No writes were sent.');
        }
      }
      const current = await withRuntime(
        (call) =>
          source.kind === 'classic'
            ? classicTarget(device, source, card, call, dictionary, onProgress, target.sectorKeys)
            : type2Target(device, source, call, credential),
        signal
      );

      const memoryChanged = target.memory.some((bytes, index) => {
        const currentBytes = current.memory[index];
        return !(bytes === null && currentBytes === null) && !bytesEqual(bytes, currentBytes);
      });
      const identityChanged = target.identityMemory.some((bytes, index) => {
        const currentBytes = current.identityMemory[index];
        return !(bytes === null && currentBytes === null) && !bytesEqual(bytes, currentBytes);
      });
      if (memoryChanged || identityChanged) {
        throw new Error('The target changed during review. No writes were sent.');
      }

      device.check();
      const writing = () => {
        onProgress(`${completed}/${total} ${unitLabel}${total === 1 ? '' : 's'} written and verified…`, {
          completed,
          total,
          unitLabel
        });
      };
      const verified = () => {
        completed++;
        writing();
      };
      if (cloning) {
        // Repeat all security checks after review and before the first write.
        clone.credentials = await checkTrailers(device, model, card.uid, clone.trailers, current.sectorKeys);
        if (clone.identity) {
          const before = await device.command(COMMAND.SET_BLOCK0, [
            ...writePayload(model, card.uid, clone.identity, clone.credential),
            1
          ]);
          if (!bytesEqual(before, clone.identity.before)) {
            throw new Error('Block 0 changed during review');
          }
        }
      }
      if (total && bytesEqual(source.classicRead?.card.uid, card.uid)) {
        delete source.classicRead;
        if (cloning) {
          delete source.recoveredKeys;
          delete source.classicObservation;
        }
      }
      writing();
      await writeUnits(device, model, card.uid, plan.writes, target.credentials, onProgress, verified);
      if (cloning) {
        // Change identity while the original sector-0 keys and permissions still apply.
        if (clone.identity) {
          device.check();
          onProgress('Writing and verifying block 0…');
          const reply = await device.writeCommand(
            COMMAND.SET_BLOCK0,
            writePayload(model, card.uid, clone.identity, clone.credential)
          );
          if (!bytesEqual(reply, clone.identity.after)) {
            throw new Error('Block 0 verification mismatch');
          }
          verified();
        }
        await writeTrailers(
          device,
          model,
          source.card.uid,
          clone.trailers,
          clone.credentials,
          onProgress,
          verified
        );
        onProgress('Checking target radio identity…');
        const observed = await scanNfcA(device);
        if (
          !bytesEqual(observed.uid, source.card.uid) ||
          !bytesEqual(observed.atqa, source.card.atqa) ||
          observed.sak !== source.card.sak ||
          (observed.ats?.length && !bytesEqual(observed.ats, source.card.ats || []))
        ) {
          throw new Error('The target radio identity does not match the source');
        }
        // The Classic scan can omit RATS. Obtain fresh bound ATS evidence for
        // SL1 targets before comparing activation data after cloning.
        observed.ats = await classicCopyCapacity(device, source, observed, onProgress);
        if (!bytesEqual(observed.ats, source.card.ats || [])) {
          throw new Error('The target radio identity does not match the source');
        }
      }
      onProgress(
        cloning
          ? 'Clone verified · data, block 0, keys and access rights match'
          : `${completed} ${unitLabel}${completed === 1 ? '' : 's'} written and verified · data copy`
      );
    });
  } catch (error) {
    if (!total) {
      throw error;
    }
    throw new Error(
      `${completed}/${total} blocks written and verified. ${error.message} Inspect the target before retrying.`
    );
  }
}

export async function changeTagUid(signal, onProgress, source, { requestUid }) {
  const device = new Device(signal);
  return withDevice(device, async () => {
    onProgress('Present the tag whose UID you want to change…');
    const card = await device.begin();
    if (card.uid.length !== 4 || ![8, 9, 16, 24].includes(card.sak & ~0x20)) {
      throw new Error('UID change currently supports four-byte Classic magic tags');
    }

    let magic = await device.probeMagic(card);
    let credential = null;
    const keys = classicObservationMatches(source?.classicObservation, card)
      ? source.recoveredKeys?.[0]
      : null;
    if (!magicGen1aDetected(magic) && magic.gen2 !== GEN2_INCONCLUSIVE && keys) {
      for (const keyB of [false, true]) {
        const key = keyB ? keys.key_b : keys.key_a;
        if (!key || (keyB && keys.origin_b === 'Trailer')) {
          continue;
        }
        if (!magicGen2Confirmed(magic)) {
          magic = await device.probeMagic(card, key, keyB);
        }
        if (magicGen2Confirmed(magic)) {
          credential = { key, keyB };
          break;
        }
        if (magic.gen2 === GEN2_INCONCLUSIVE) {
          break;
        }
      }
    }
    if (
      !magicGen1aDetected(magic) &&
      !credential &&
      classicObservationMatches(source?.classicObservation, card)
    ) {
      for (const saved of source.classicObservation.credentials || []) {
        magic = await device.probeMagic(card, saved.key, saved.keyB);
        if (magicGen2Confirmed(magic)) {
          credential = saved;
          break;
        }
      }
    }
    device.check();
    const selection = await requestUid({ card, magic, credential, suggestedUid: source?.card.uid }, signal);
    if (!selection) {
      onProgress('UID change cancelled');
      return;
    }

    const { uid, key, keyB } = selection;
    let suffix = [];
    if (key) {
      if (!(await device.findKey(0, keyB, [key], { sectors: [{ first: 0 }] }))) {
        throw new Error('Sector-0 key was rejected. No write was sent.');
      }
      suffix = [keyB ? 2 : 1, ...key];
    }

    onProgress('Writing the new UID…');
    delete source?.classicRead;
    delete source?.classicObservation;
    const reply = await device.writeCommand(COMMAND.SET_UID, [
      4,
      ...card.uid,
      0,
      0,
      0,
      ...uid,
      0,
      0,
      0,
      ...suffix
    ]);
    if (reply.length !== 5 || reply[0] !== 4 || !bytesEqual(reply.slice(1), uid)) {
      throw new Error('UID verification mismatch. Inspect the tag before retrying.');
    }
    onProgress(`UID changed and verified: ${hex(uid)}. Scan the tag to refresh this document.`);
  });
}
