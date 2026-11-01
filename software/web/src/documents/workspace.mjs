import { desfireDocument, desfireImage, importDesfireImage } from './desfire.mjs';
import { hfImage, hfImageDocument, captureDocument, restoreHfDocument } from './hf-image.mjs';
import { withRuntime } from '../runtime/runtime.mjs';
import { hex } from '../device/transport.mjs';
import {
  bytesValid,
  encodeSlotImage,
  family,
  LF_RECORD_BYTES,
  TYPE2_PROFILES,
  typeName
} from './slot-image.mjs';

export const TYPE2_NOTES = {
  credentials: 'Password/key pages are not readable; original credentials remain unknown',
  metadata: 'Some counter/signature metadata is unread',
  defaults:
    'Unread credentials/metadata replaced with emulator defaults; original authentication and missing originality data are not reproduced'
};

export function parseHex(text, length) {
  const clean = text.replace(/\s/g, '');
  if (clean.length !== length * 2 || !/^[0-9a-f]+$/i.test(clean)) {
    throw new Error(`Enter exactly ${length} bytes in hexadecimal`);
  }
  return Array.from(clean.match(/../g), (byte) => parseInt(byte, 16));
}

export function editBlock(document, block, bytes) {
  const classic = (document.kind ?? 'classic') === 'classic';
  if (
    !Number.isInteger(block) ||
    block < 0 ||
    block >= document.memory.length ||
    bytes.length !== (document.unitBytes ?? 16) ||
    !(classic ? partialBlock(bytes) : bytesValid(bytes))
  ) {
    throw new Error('Invalid memory block');
  }

  document.memory[block] = [...bytes];
  if (block === 0 && classic && document.card.uid.length === 4 && bytesValid(bytes.slice(0, 4))) {
    document.card.uid = bytes.slice(0, 4);
  }
  if (
    document.kind === 'type2' &&
    block < 2 &&
    document.card.uid.length === 7 &&
    document.memory[0] &&
    document.memory[1]
  ) {
    document.card.uid = [...document.memory[0].slice(0, 3), ...document.memory[1]];
  }
}

export async function blankClassic(sak, uid, signal) {
  if (!bytesValid(uid) || uid.length !== 4) {
    throw new Error('New Classic documents require a 4-byte UID');
  }
  return withRuntime(async (call) => {
    const geometry = await call({ op: 'classic_geometry', sak });
    const memory = Array.from({ length: geometry.blocks }, () => Array(16).fill(0));
    memory[0].splice(
      0,
      8,
      ...uid,
      uid.reduce((checksum, byte) => checksum ^ byte),
      geometry.sak,
      geometry.atqa[1],
      geometry.atqa[0]
    );
    for (const sector of geometry.sectors) {
      memory[sector.trailer] = [...Array(6).fill(255), 255, 7, 128, 105, ...Array(6).fill(255)];
    }

    const type = { 20: 1000, 64: 1001, 128: 1002, 256: 1003 }[geometry.blocks];
    return {
      name: `${geometry.name}-${hex(uid)}`,
      type,
      kind: 'classic',
      unitBytes: 16,
      card: { uid, sak: geometry.sak, atqa: geometry.atqa },
      geometry,
      memory
    };
  }, signal);
}

export function type2Document(type, card, geometry) {
  const hasVersion = geometry.version.length > 0;
  const counterCount = hasVersion ? Math.max(1, geometry.counters.length) : 0;
  const extra = Array(counterCount + (hasVersion ? 10 : 0)).fill(null);
  if (hasVersion) {
    for (let page = 0; page < 2; page++) {
      extra[counterCount + page] = geometry.version.slice(page * 4, page * 4 + 4);
    }
  }
  return {
    name: `${typeName(type)}-${hex(card.uid)}`,
    type,
    kind: 'type2',
    unitBytes: 4,
    card,
    geometry,
    memory: Array(geometry.pages).fill(null),
    emulator: { options: [0, 0, 0, geometry.pages >> 8, geometry.pages & 255], extra },
    warnings: []
  };
}

// NFC Forum CC sizes in eight-byte units. Use the manufacturer's declared NDEF
// area for NTAG215/216, not total EEPROM (NXP NTAG213_215_216, section 8.5.6).
const TYPE2_CC_SIZE = {
  1100: 0x12,
  1101: 0x3e,
  1102: 0x6d,
  1103: 0x06,
  1104: 0x12,
  1105: 0x06,
  1106: 0x10,
  1107: 0x06,
  1108: 0x10
};

export async function blankType2(type, uid, signal) {
  if (!TYPE2_PROFILES[type] || !bytesValid(uid) || uid.length !== 7) {
    throw new Error('Choose a supported Type 2 model and a 7-byte UID');
  }
  return withRuntime(async (call) => {
    const geometry = await call({ op: 'type2_geometry', profile: TYPE2_PROFILES[type] });
    const document = type2Document(type, { uid: [...uid], atqa: [0, 0x44], sak: 0, ats: [] }, geometry);
    const memory = Array.from({ length: geometry.pages }, () => [0, 0, 0, 0]);
    memory[0] = [...uid.slice(0, 3), 0x88 ^ uid[0] ^ uid[1] ^ uid[2]];
    memory[1] = uid.slice(3);
    memory[2] = [uid.slice(3).reduce((bcc, byte) => bcc ^ byte, 0), 0x48, 0, 0];
    memory[3] = [0xe1, 0x10, TYPE2_CC_SIZE[type], 0];
    memory[4] = [0x03, 0, 0xfe, 0];
    if (type === 1100) {
      memory[4] = [0x01, 0x03, 0xa0, 0x0c];
      memory[5] = [0x34, 0x03, 0, 0xfe];
    }

    if (geometry.version.length) {
      const config = geometry.pages - 4;
      memory[config][3] = 0xff;
      memory[config + 2] = [255, 255, 255, 255];
      if ([1100, 1101, 1102].includes(type)) {
        memory[config][0] = 0x04;
      } else if (geometry.tearing) {
        memory[config + 1][1] = 0x05;
      }
    } else if (type === 1104) {
      memory[42][0] = 0x30;
      // Ultralight C's default 3DES key, in on-card byte order.
      const key = [
        0x49, 0x45, 0x4d, 0x4b, 0x41, 0x45, 0x52, 0x42, 0x21, 0x4e, 0x41, 0x43, 0x55, 0x4f, 0x59, 0x46
      ];
      for (let page = 0; page < 4; page++) {
        memory[44 + page] = key.slice(page * 4, page * 4 + 4);
      }
    }

    document.memory = memory;
    document.emulator.extra = document.emulator.extra.map((page) => page || [0, 0, 0, 0]);
    document.warnings = ['Created with emulator defaults and empty NDEF; no original tag signature'];
    encodeSlotImage(document);
    return document;
  }, signal);
}

const TYPE2_MODELS = {
  2: 1101,
  8: 1100,
  9: 1102,
  10: 1103,
  11: 1105,
  12: 1106,
  19: 1104,
  20: 1107,
  21: 1108
};

export function canExportNfc(document) {
  switch (document?.kind) {
    case 'desfire':
      return !!document.capture?.complete;
    case 'st25tb':
      return document.type === 2000 && !!(document.card.system ?? document.capture?.system);
    case 'nfcv':
      return !!document.emulator;
    case 'type2':
      return Object.values(TYPE2_MODELS).includes(document.type) && document.card.uid.length === 7;
    default:
      return (!document?.kind || document.kind === 'classic') && document?.card?.uid.length === 4;
  }
}

export async function importNfc(text, name, signal) {
  return withRuntime(async (call) => {
    const { image, warnings } = await call({ op: 'import_flipper', text });
    if ([14, 18].includes(image.model)) {
      return importDesfireImage(image, name);
    }
    if ([3, 4, 13, 22].includes(image.model)) {
      return hfImageDocument(image, name, warnings);
    }

    if (TYPE2_MODELS[image.model]) {
      const type = TYPE2_MODELS[image.model];
      const geometry = await call({ op: 'type2_geometry', profile: TYPE2_PROFILES[type] });
      const uid = image.uid;
      const memory = Array.from({ length: geometry.pages }, (_, page) =>
        image.memory.slice(page * 4, page * 4 + 4)
      );
      const metadata = image.metadata;
      const extra = [];

      if (geometry.version.length) {
        const attempts = metadata
          .slice(56, 60)
          .reduce((value, byte, index) => value + byte * 2 ** (8 * index), 0);
        if (attempts > 15) {
          throw new Error('The authentication attempt count exceeds the Chameleon emulator limit');
        }

        for (const index of geometry.counters) {
          const tearing = metadata[53 + index];
          if (![0, 0xbd].includes(tearing)) {
            throw new Error('Unsupported Type 2 tearing state');
          }
          extra.push([...metadata.slice(44 + index * 3, 47 + index * 3), tearing === 0xbd ? 0 : 0x80]);
        }
        if (!geometry.counters.length) {
          extra.push([0, 0, 0, 0]);
        }
        extra[0][3] |= attempts;

        for (const bytes of [metadata.slice(4, 12), metadata.slice(12, 44)]) {
          for (let offset = 0; offset < bytes.length; offset += 4) {
            extra.push(bytes.slice(offset, offset + 4));
          }
        }

        if (
          geometry.counters.length < 3 &&
          metadata.slice(44, geometry.counters.length ? 50 : 53).some(Boolean)
        ) {
          warnings.push('Unused NTAG counter values are not represented by this emulator');
        }
      }

      const document = type2Document(type, { uid, atqa: [0, 0x44], sak: 0, ats: [] }, geometry);
      document.name = name;
      document.memory = memory;
      document.emulator.extra = extra;
      document.warnings = warnings;
      encodeSlotImage(document);
      return document;
    }

    const sak = new Map([
      [1, 8],
      [5, 9],
      [6, 16],
      [7, 24]
    ]).get(image.model);
    if (sak === undefined) {
      throw new Error('This workspace imports Classic and supported Type 2 images');
    }

    const geometry = await call({ op: 'classic_geometry', sak });
    const uid = image.uid;
    const data = image.memory;
    if (uid.length !== 4 || data.length !== geometry.blocks * 16) {
      throw new Error('Unsupported image geometry');
    }

    const memory = Array.from({ length: geometry.blocks }, (_, block) =>
      data.slice(block * 16, block * 16 + 16)
    );
    const type = { 20: 1000, 64: 1001, 128: 1002, 256: 1003 }[geometry.blocks];
    return {
      name,
      type,
      kind: 'classic',
      unitBytes: 16,
      card: { uid, sak: geometry.sak, atqa: [memory[0][7], memory[0][6]] },
      geometry,
      memory,
      warnings
    };
  }, signal);
}

export async function exportNfc(document, signal) {
  if (document.kind === 'desfire') {
    return withRuntime(
      async (call) => (await call({ op: 'export_flipper', image: desfireImage(document) })).text,
      signal
    );
  }

  if (['st25tb', 'nfcv'].includes(document.kind)) {
    const image = hfImage(document);
    return withRuntime(async (call) => (await call({ op: 'export_flipper', image })).text, signal);
  }

  if (document.kind !== 'type2') {
    return exportClassic(document, signal);
  }
  if (!canExportNfc(document)) {
    throw new Error('This Type 2 profile or UID cannot be exported as .nfc');
  }

  encodeSlotImage(document);
  return withRuntime(async (call) => {
    const geometry = await call({ op: 'type2_geometry', profile: TYPE2_PROFILES[document.type] });
    if (document.memory.length !== geometry.pages) {
      throw new Error('Type 2 memory does not match its profile');
    }

    const metadata = geometry.version.length ? Array(60).fill(0) : [];
    if (metadata.length) {
      const extra = document.emulator.extra;
      const counterCount = Math.max(1, geometry.counters.length);
      if (extra.length !== counterCount + 10) {
        throw new Error('Invalid Type 2 metadata');
      }

      metadata.splice(0, 4, 78, 84, 65, 49);
      metadata.splice(4, 8, ...extra.slice(counterCount, counterCount + 2).flat());
      metadata.splice(12, 32, ...extra.slice(counterCount + 2).flat());
      metadata.splice(53, 3, 0xbd, 0xbd, 0xbd);
      for (const [extraIndex, counterIndex] of geometry.counters.entries()) {
        metadata.splice(44 + counterIndex * 3, 3, ...extra[extraIndex].slice(0, 3));
        metadata[53 + counterIndex] = extra[extraIndex][3] & 0x80 ? 0 : 0xbd;
      }
      metadata[56] = extra[0][3] & 15;
    }

    const bytes = document.memory.flat();
    const model = Number(Object.keys(TYPE2_MODELS).find((model) => TYPE2_MODELS[model] === document.type));
    const image = { model, uid: document.card.uid, memory: bytes, metadata, secrets: [] };
    return (await call({ op: 'export_flipper', image })).text;
  }, signal);
}

export async function exportClassic(document, signal) {
  if (!document.memory?.length || !document.memory.every(knownBlock)) {
    throw new Error('Complete .nfc export requires every block to be known');
  }

  return withRuntime(async (call) => {
    const result = await call({
      op: 'classic_export',
      uid: document.card.uid,
      sak: classicSelector(document),
      memory: document.memory.flat()
    });
    return result.text;
  }, signal);
}

export function classicSelector(document) {
  return document.type >= 1000 && document.type <= 1003
    ? [9, 8, 16, 24][document.type - 1000]
    : document.card.sak;
}

export const knownBlock = (block) => bytesValid(block);

export const partialBlock = (block) =>
  Array.isArray(block) &&
  block.every((byte) => byte === null || (Number.isInteger(byte) && byte >= 0 && byte <= 255));

export const memoryHex = (block) => block.map((byte) => (byte === null ? '??' : hex([byte]))).join('');

export function parseMemoryHex(text, length) {
  const clean = text.replace(/\s/g, '');
  if (clean.length !== length * 2 || !/^([\da-f]{2}|\?\?)+$/i.test(clean)) {
    throw new Error(`Enter ${length} hex bytes, using ?? for unread bytes`);
  }
  return clean.match(/../g).map((pair) => (pair === '??' ? null : parseInt(pair, 16)));
}

export async function inspectMct(text, signal) {
  return withRuntime((call) => call({ op: 'mct_parse', text }), signal);
}

export async function importMct(text, name, sak, uid, signal) {
  if (!bytesValid(uid) || ![4, 7, 10].includes(uid.length)) {
    throw new Error('Enter a 4, 7 or 10-byte UID');
  }

  return withRuntime(async (call) => {
    const dump = await call({ op: 'mct_parse', text });
    const geometry = await call({ op: 'classic_geometry', sak });
    if (dump.sectors.some((sector) => sector >= geometry.sectors.length)) {
      throw new Error('MCT sectors exceed the selected card size');
    }

    const memory = dump.memory
      .slice(0, geometry.blocks)
      .map((block) => (block.every((byte) => byte === null) ? null : block));
    const type = { 20: 1000, 64: 1001, 128: 1002, 256: 1003 }[geometry.blocks];
    const atqa = [...geometry.atqa];
    atqa[1] = (atqa[1] & 0x3f) | { 4: 0, 7: 0x40, 10: 0x80 }[uid.length];
    return {
      name,
      type,
      kind: 'classic',
      unitBytes: 16,
      card: { uid, sak: geometry.sak, atqa },
      geometry,
      memory,
      warnings: ['MCT has no reliable model/UID metadata; using the identity you confirmed']
    };
  }, signal);
}

export async function exportMct(document, signal) {
  if (document.kind && document.kind !== 'classic') {
    throw new Error('MCT supports Classic memory only');
  }
  const memory = document.memory.map((block) => block || Array(16).fill(null));
  return withRuntime(async (call) => (await call({ op: 'mct_export', memory })).text, signal);
}

export function serializeDraft(document) {
  const {
    name,
    card,
    memory,
    type,
    kind,
    unitBytes,
    emulator,
    warnings,
    locks,
    capture,
    recoveredKeys,
    lf,
    unbound
  } = document;
  return JSON.stringify(
    {
      version: 1,
      document: {
        name,
        card,
        memory,
        type,
        kind,
        unitBytes,
        emulator,
        warnings,
        locks,
        capture,
        unbound,
        recoveredKeys,
        lf
      }
    },
    (_, value) => {
      if (!value || typeof value !== 'object' || Array.isArray(value)) {
        return value;
      }
      return Object.fromEntries(
        Object.entries(value).sort(([firstKey], [secondKey]) => firstKey.localeCompare(secondKey))
      );
    }
  );
}

export async function restoreDraft(text, signal) {
  if (!text || text.length > 100000) {
    return null;
  }

  const saved = JSON.parse(text);
  if (saved.version === 1 && saved.protocol === 'desfire') {
    return desfireDocument(saved);
  }
  if (saved.version === 1 && saved.tag_type === 1300) {
    const wire = new Uint8Array([
      2,
      5,
      20,
      saved.memory.length >> 8,
      saved.memory.length & 255,
      ...saved.memory
    ]);
    return (await import('./slot-image.mjs')).decodeSlotImage(wire, 'DESFire');
  }
  if (saved.document?.kind === 'desfire') {
    return desfireDocument(saved.document.capture, saved.document.name);
  }
  if (saved.version === 1 && ['st25tb', 'nfc_v'].includes(saved.protocol)) {
    return captureDocument(saved);
  }
  if (saved.version !== 1 || !saved.document) {
    throw new Error('Invalid browser document');
  }

  const draft = saved.document;
  if (draft.card?.protocol) {
    return restoreHfDocument(draft);
  }

  let kind;
  if (draft.kind === 'identity') {
    kind = 'identity';
  } else if (draft.kind === 'lf' && draft.type === undefined && draft.lf?.chip) {
    kind = 'lf';
  } else {
    kind = draft.type === undefined ? 'classic' : family(draft.type);
  }
  const unitBytes = draft.unitBytes ?? 16;

  if (
    typeof draft.name !== 'string' ||
    !draft.card ||
    !kind ||
    !bytesValid(draft.card.uid) ||
    !(kind === 'lf' ? draft.card.uid.length === 0 : [4, 7, 10].includes(draft.card.uid.length)) ||
    !Number.isInteger(draft.card.sak) ||
    draft.card.sak < 0 ||
    draft.card.sak > 255 ||
    (draft.card.ats !== undefined && (!bytesValid(draft.card.ats) || draft.card.ats.length > 255)) ||
    !bytesValid(draft.card.atqa) ||
    draft.card.atqa.length !== (kind === 'lf' ? 0 : 2) ||
    !Number.isInteger(unitBytes) ||
    unitBytes < 1 ||
    unitBytes > 16
  ) {
    throw new Error('Invalid browser draft');
  }

  const validBlock = kind === 'classic' ? partialBlock : bytesValid;
  if (
    !Array.isArray(draft.memory) ||
    (kind !== 'identity' &&
      !draft.memory.length &&
      !(kind === 'lf' && draft.type === undefined && draft.lf?.chip)) ||
    draft.memory.length > 256 ||
    draft.memory.some((block) => block !== null && (!validBlock(block) || block.length !== unitBytes))
  ) {
    throw new Error('Invalid browser draft');
  }
  if (
    kind === 'identity' &&
    (draft.type !== undefined || draft.memory.length || draft.emulator || unitBytes !== 16)
  ) {
    throw new Error('Invalid identity draft');
  }

  // A recovered draft is local data; it never restores a hardware reservation.
  let geometry;
  if (kind === 'classic') {
    geometry = await withRuntime(
      (call) => call({ op: 'classic_geometry', sak: classicSelector(draft) }),
      signal
    );
    if (unitBytes !== 16 || geometry.blocks !== draft.memory.length) {
      throw new Error('Draft memory does not match its card geometry');
    }
  }

  const recoveredKeys = draft.recoveredKeys;
  if (
    recoveredKeys !== undefined &&
    (kind !== 'classic' ||
      !Array.isArray(recoveredKeys) ||
      recoveredKeys.length !== geometry.sectors.length ||
      recoveredKeys.some(
        (keys) =>
          !keys ||
          ['key_a', 'key_b'].some(
            (key) => keys[key] !== null && (!bytesValid(keys[key]) || keys[key].length !== 6)
          )
      ))
  ) {
    throw new Error('Invalid recovered keys');
  }

  if (kind === 'type2') {
    geometry = await withRuntime(
      (call) => call({ op: 'type2_geometry', profile: TYPE2_PROFILES[draft.type] }),
      signal
    );
    const options = draft.emulator?.options;
    const extra = draft.emulator?.extra;
    const hasVersion = geometry.version.length > 0;
    const counterCount = hasVersion ? Math.max(1, geometry.counters.length) : 0;
    if (
      unitBytes !== 4 ||
      draft.memory.length !== geometry.pages ||
      !bytesValid(options) ||
      options.length !== 5 ||
      options[0] > 1 ||
      options[1] > 4 ||
      options[2] > 1 ||
      options[3] * 256 + options[4] !== geometry.pages ||
      !Array.isArray(extra) ||
      extra.length !== counterCount + (hasVersion ? 10 : 0) ||
      extra.some((page) => page !== null && (!bytesValid(page) || page.length !== 4))
    ) {
      throw new Error('Invalid Type 2 draft');
    }
  }

  const canValidateSlot =
    kind !== 'identity' &&
    draft.memory.length > 0 &&
    draft.memory.every(knownBlock) &&
    (kind === 'lf' || (draft.emulator?.extra || []).every(Boolean));
  if (canValidateSlot) {
    encodeSlotImage(draft);
  }

  const lf = draft.lf;
  if (lf !== undefined) {
    const validWord = (value) =>
      value === null || (Number.isInteger(value) && value >= 0 && value <= 0xffffffff);
    if (
      kind !== 'lf' ||
      !lf ||
      (lf.input !== undefined &&
        (!lf.input ||
          Array.isArray(lf.input) ||
          draft.memory[0] !== null ||
          Object.entries(lf.input).some(
            ([key, value]) =>
              !['id', 'epilogue', 'format', 'facility', 'number', 'issue', 'oem', 'version', 'raw'].includes(
                key
              ) ||
              typeof value !== 'string' ||
              value.length > 128
          ))) ||
      (lf.clock !== undefined && ![16, 32, 64].includes(lf.clock)) ||
      (lf.chip !== undefined &&
        lf.chip !== null &&
        (!Array.isArray(lf.chip) ||
          lf.chip.length !== 7 ||
          !lf.chip.every(validWord) ||
          lf.chip.slice(0, 3).some((value) => value === null)))
    ) {
      throw new Error('Invalid recorded LF observation');
    }
  }

  if (
    kind === 'lf' &&
    draft.type !== undefined &&
    (draft.memory.length !== 1 || unitBytes !== LF_RECORD_BYTES[draft.type])
  ) {
    throw new Error('Invalid RFID record geometry');
  }

  const { uid, sak, atqa, ats = [] } = draft.card;
  return {
    name: draft.name,
    type: draft.type,
    kind,
    unitBytes,
    card: { uid, sak, atqa, ats },
    memory: draft.memory,
    emulator: draft.emulator,
    geometry,
    recoveredKeys,
    lf,
    warnings: Array.isArray(draft.warnings)
      ? draft.warnings.filter((warning) => typeof warning === 'string').slice(0, 8)
      : []
  };
}

export function needsType2Defaults(document) {
  return (
    document?.kind === 'type2' &&
    !!document.geometry?.credentials &&
    (document.geometry.credentials.some((page) => !document.memory[page]) ||
      document.emulator.extra.some((page) => !page))
  );
}

export function applyType2Defaults(document) {
  if (!needsType2Defaults(document)) {
    return;
  }

  for (const page of document.geometry.credentials) {
    if (!document.memory[page]) {
      const passwordPage = document.geometry.version.length && page === document.geometry.credentials[0];
      document.memory[page] = Array(4).fill(passwordPage ? 255 : 0);
    }
  }

  document.emulator.extra = document.emulator.extra.map((page) => page || [0, 0, 0, 0]);
  document.warnings.push(TYPE2_NOTES.defaults);
}
