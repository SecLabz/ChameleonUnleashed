import { isByte } from './slot-image.mjs';
import { ST25TB, st25tbName } from '../device/st25tb-protocol.mjs';
import { hex } from '../device/transport.mjs';
import { validateNfcvSlot } from './nfcv-slot.mjs';

export const HF_NOTES = {
  chipId: 'Emulator Chip ID was explicitly chosen; it was not observed from the tag',
  missingChipId: 'Flipper files do not contain the emulator Chip ID; choose one before saving to a slot',
  credentials: 'Some security metadata and passwords may remain unknown',
  metadata:
    'Unknown locks are unlocked; hidden credentials and unread security metadata use emulator defaults. The capture is unchanged.'
};

const validBytes = (value, count) => Array.isArray(value) && value.length === count && value.every(isByte);

const optionalByte = (value) => value === null || isByte(value);

const validLock = (value) => value === null || typeof value === 'boolean';

const knownBlock = (bytes) => bytes?.length === 4 && bytes.every(isByte);

function hfName(card) {
  if (card.protocol === 'nfc_b') {
    return 'ISO14443-B';
  }
  if (card.protocol === 'st25tb') {
    return st25tbName(card.uid);
  }
  if (card.uid?.slice(0, 3).join() === [0xe0, 4, 1].join() && card.blocks === 28 && card.block_size === 4) {
    return 'SLIX';
  }
  if (card.uid?.slice(0, 3).join() === [0xe0, 4, 3].join() && card.blocks === 8 && card.block_size === 4) {
    return 'SLIX-L';
  }
  return 'ISO15693';
}

export function hfDocument(card) {
  const name = hfName(card);
  const readable = card.blocks > 0 && card.blocks <= 64 && card.block_size === 4;
  const memoryBlocks = readable ? card.blocks : 0;
  let type;
  let kind = 'nfcv';

  if (card.protocol === 'st25tb') {
    kind = 'st25tb';
    if (name === 'ST25TB512-AT' && card.blocks === ST25TB.BLOCKS) {
      type = ST25TB.TYPE;
    }
  } else if (card.protocol === 'nfc_b') {
    kind = 'identity';
  } else if (card.protocol === 'nfc_v') {
    type = { SLIX: 1501, 'SLIX-L': 1500 }[name];
  }

  return {
    type,
    name: `${name}-${hex(card.uid || card.pupi)}`,
    kind,
    unitBytes: 4,
    card,
    geometry: { name, blocks: card.blocks || 0 },
    memory: Array(memoryBlocks).fill(null),
    locks: Array(memoryBlocks).fill(null),
    warnings: readable ? [] : ['Identity captured; memory reading is unsupported for this geometry']
  };
}

export function blankNfcv(type, uid) {
  const blocks = { 1500: 8, 1501: 28 }[type];
  if (!blocks) {
    throw new Error('This NFC-V model cannot be created');
  }
  if (uid === undefined) {
    uid = Array.from(crypto.getRandomValues(new Uint8Array(8)));
    uid.splice(0, 3, 0xe0, 4, type === 1500 ? 3 : 1);
    if (type === 1501) {
      uid[3] = (uid[3] & ~0x18) | 0x10;
    }
  }

  if (
    !validBytes(uid, 8) ||
    uid[0] !== 0xe0 ||
    uid[1] !== 4 ||
    uid[2] !== (type === 1500 ? 3 : 1) ||
    (type === 1501 && (uid[3] & 0x18) !== 0x10)
  ) {
    throw new Error('Enter an eight-byte UID matching the selected SLIX model');
  }

  const document = hfDocument({
    protocol: 'nfc_v',
    uid: [...uid],
    blocks,
    block_size: 4,
    dsfid: null,
    afi: null,
    ic_ref: null
  });
  document.name = document.geometry.name;
  document.unbound = true;
  document.warnings = ['The first read adopts the presented tag’s UID; unread memory remains unknown'];
  return document;
}

function validateCapture(capture) {
  if (
    !capture ||
    capture.version !== 1 ||
    !['st25tb', 'nfc_v'].includes(capture.protocol) ||
    !validBytes(capture.uid, 8) ||
    !Array.isArray(capture.memory) ||
    !capture.memory.length ||
    capture.memory.length > 64 ||
    (capture.protocol === 'st25tb' && capture.memory.length !== ST25TB.BLOCKS) ||
    capture.memory.some((block) => block !== null && !knownBlock(block)) ||
    !Array.isArray(capture.locks) ||
    capture.locks.length !== capture.memory.length ||
    !capture.locks.every(validLock) ||
    !['dsfid', 'afi', 'ic_ref', 'chip_id', 'tag_error'].every((key) => optionalByte(capture[key])) ||
    (capture.system !== null && !validBytes(capture.system, 4)) ||
    !isByte(capture.status) ||
    typeof capture.complete !== 'boolean' ||
    typeof capture.deadline !== 'boolean'
  ) {
    throw new Error('Invalid NFC reader capture');
  }

  const invalidCompletion =
    capture.complete &&
    (capture.deadline || capture.status || capture.memory.some((block) => !knownBlock(block)));
  const invalidSt25tbMetadata =
    capture.protocol === 'st25tb' &&
    (capture.chip_id === null ||
      capture.dsfid !== null ||
      capture.afi !== null ||
      capture.ic_ref !== null ||
      capture.tag_error !== null ||
      (capture.complete && capture.system === null));
  const invalidNfcvMetadata =
    capture.protocol === 'nfc_v' && (capture.system !== null || capture.chip_id !== null);
  if (invalidCompletion || invalidSt25tbMetadata || invalidNfcvMetadata) {
    throw new Error('Invalid NFC reader capture');
  }
  return capture;
}

export function captureDocument(capture) {
  validateCapture(capture);

  const card = {
    protocol: capture.protocol,
    uid: [...capture.uid],
    blocks: capture.memory.length,
    block_size: 4,
    chip_id: capture.chip_id,
    dsfid: capture.dsfid,
    afi: capture.afi,
    ic_ref: capture.ic_ref,
    ...(capture.protocol === 'st25tb' ? { system: capture.system } : {})
  };
  const document = hfDocument(card);
  document.memory = capture.memory.map((block) => (block ? [...block] : null));
  document.locks = [...capture.locks];
  document.capture = capture;
  if (!capture.complete) {
    document.warnings.push('Partial capture; unread blocks remain unknown');
  }
  return document;
}

export function restoreHfDocument(doc) {
  const card = doc?.card;
  if (
    !card ||
    !['nfc_b', 'st25tb', 'nfc_v'].includes(card.protocol) ||
    typeof doc.name !== 'string' ||
    doc.name.length > 256 ||
    (doc.type !== hfDocument(card).type && !(card.protocol === 'nfc_v' && doc.type === undefined)) ||
    doc.unitBytes !== 4
  ) {
    throw new Error('Invalid NFC reader document');
  }

  if (card.protocol === 'nfc_b') {
    if (!validBytes(card.pupi, 4) || !validBytes(card.application, 4) || !validBytes(card.protocol_info, 3)) {
      throw new Error('Invalid NFC-B identity');
    }
  } else if (
    !validBytes(card.uid, 8) ||
    (card.block_size !== null &&
      (!Number.isInteger(card.block_size) || card.block_size < 1 || card.block_size > 32)) ||
    (card.blocks !== null && (!Number.isInteger(card.blocks) || card.blocks < 0 || card.blocks > 256)) ||
    (card.protocol === 'st25tb' &&
      (!optionalByte(card.chip_id) ||
        ![0, ST25TB.BLOCKS].includes(card.blocks) ||
        (card.system !== undefined && card.system !== null && !validBytes(card.system, 4))))
  ) {
    throw new Error('Invalid NFC tag identity');
  }

  if (doc.emulator !== undefined) {
    const emulator = doc.emulator;
    const count = { 3: 28, 4: 8, 13: 64 }[emulator?.model];
    const secretBytes = { 3: 4, 4: 12 }[emulator?.model] ?? 0;
    if (
      card.protocol !== 'nfc_v' ||
      count !== card.blocks ||
      card.block_size !== 4 ||
      !validBytes(emulator.metadata, 12 + count) ||
      !validBytes(emulator.secrets, secretBytes) ||
      emulator.metadata.slice(0, 4).join() !== '86,73,67,49' ||
      emulator.metadata.slice(8, 12).join() !== [4, count, 0, 0].join() ||
      emulator.metadata.slice(12).some((value) => value > 1)
    ) {
      throw new Error('Invalid NFC-V emulator metadata');
    }
  }

  const result = hfDocument(card);
  if (
    doc.kind !== result.kind ||
    !Array.isArray(doc.memory) ||
    doc.memory.length !== result.memory.length ||
    doc.memory.some((block) => block !== null && !knownBlock(block)) ||
    !Array.isArray(doc.locks) ||
    doc.locks.length !== doc.memory.length ||
    !doc.locks.every(validLock)
  ) {
    throw new Error('Invalid NFC reader memory');
  }

  if (doc.capture) {
    validateCapture(doc.capture);
    if (
      doc.capture.protocol !== card.protocol ||
      doc.capture.uid.join() !== card.uid.join() ||
      doc.capture.memory.length !== doc.memory.length
    ) {
      throw new Error('Capture identity does not match document');
    }
    result.capture = doc.capture;
  }

  if (doc.unbound !== undefined) {
    if (doc.unbound !== true || ![1500, 1501].includes(result.type) || doc.capture) {
      throw new Error('Invalid unbound NFC-V document');
    }
    result.unbound = true;
  }

  if (doc.emulator?.legacy !== undefined) {
    validateNfcvSlot(doc.emulator.legacy, result.type);
  }
  if (doc.emulator !== undefined) {
    result.emulator = doc.emulator;
  }

  result.name = doc.name;
  result.memory = doc.memory;
  result.locks = doc.locks;
  result.warnings = Array.isArray(doc.warnings)
    ? doc.warnings.filter((warning) => typeof warning === 'string').slice(0, 8)
    : [];
  return result;
}

const bytes = (value) => Array.isArray(value) && value.every(isByte);

const countForModel = (model) => ({ 3: 28, 4: 8, 13: 64 })[model];

function modelForCard(card) {
  if (card.block_size !== 4) {
    return undefined;
  }

  const uidPrefix = card.uid.slice(0, 3).join();
  if (uidPrefix === '224,4,1' && card.blocks === 28) {
    return 3;
  }
  if (uidPrefix === '224,4,3' && card.blocks === 8) {
    return 4;
  }
  return card.blocks === 64 ? 13 : undefined;
}

export function needsHfDefaults(document) {
  if (document?.kind === 'st25tb') {
    return document.card.chip_id === null;
  }
  return document?.kind === 'nfcv' && !!modelForCard(document.card) && !document.emulator;
}

export function applyHfDefaults(document, chipId) {
  if (!needsHfDefaults(document)) {
    throw new Error('This tag does not need emulator defaults');
  }

  if (document.kind === 'st25tb') {
    if (!Number.isInteger(chipId) || chipId < 0 || chipId > 255) {
      throw new Error('Enter a one-byte Chip ID');
    }
    document.card.chip_id = chipId;
    document.warnings.push(HF_NOTES.chipId);
    return;
  }

  const { card } = document;
  const model = modelForCard(card);
  const secretBytes = { 3: 4, 4: 12 }[model] ?? 0;
  document.emulator = {
    model,
    metadata: [
      86,
      73,
      67,
      49,
      0,
      card.dsfid ?? 0,
      card.afi ?? 0,
      card.ic_ref ?? 0,
      4,
      card.blocks,
      0,
      0,
      ...document.locks.map((value) => (value === true ? 1 : 0))
    ],
    secrets: Array(secretBytes).fill(0)
  };
  document.warnings.push(HF_NOTES.metadata);
}

export function hfImageDocument(image, name, warnings = []) {
  const { model, uid, memory: data, metadata, secrets } = image;
  const blocks = model === 22 ? 16 : countForModel(model);
  if (!blocks || !bytes(uid) || uid.length !== 8 || !bytes(data) || data.length !== blocks * 4) {
    throw new Error('Unsupported NFC image geometry');
  }

  const memory = Array.from({ length: blocks }, (_, block) => data.slice(block * 4, block * 4 + 4));
  const card = {
    protocol: model === 22 ? 'st25tb' : 'nfc_v',
    uid,
    blocks,
    block_size: 4,
    ...(model === 22
      ? { chip_id: null, system: metadata }
      : { dsfid: metadata[5], afi: metadata[6], ic_ref: metadata[7] })
  };
  const document = hfDocument(card);
  document.name = name;
  document.memory = memory;
  document.warnings = [...warnings];

  if (model !== 22) {
    document.locks = metadata.slice(12).map((value) => !!value);
    document.emulator = { model, metadata, secrets };
  } else {
    document.warnings.push(HF_NOTES.missingChipId);
  }
  return document;
}

export function hfImage(document) {
  const { card, memory } = document;
  if (!memory.length || memory.some((block) => !bytes(block) || block.length !== 4)) {
    throw new Error('Complete .nfc export requires all user blocks; use JSON for partial data');
  }

  let model;
  let metadata;
  let secrets;

  if (card.protocol === 'st25tb') {
    if (document.type !== 2000) {
      throw new Error('Only ST25TB512-AT .nfc export is supported; use JSON for this tag');
    }

    model = 22;
    metadata = card.system ?? document.capture?.system;
    secrets = [];
    if (!bytes(metadata) || metadata.length !== 4) {
      throw new Error('ST25TB system data is unknown');
    }
  } else {
    if (!document.emulator) {
      throw new Error('Explicit emulator defaults are required for unknown security metadata');
    }

    ({ model, metadata, secrets } = document.emulator);
    if (model !== modelForCard(card)) {
      throw new Error('NFC-V image geometry does not match the tag');
    }

    metadata = [
      ...metadata.slice(0, 5),
      card.dsfid ?? metadata[5],
      card.afi ?? metadata[6],
      card.ic_ref ?? metadata[7],
      ...metadata.slice(8, 12),
      ...document.locks.map((value, index) => (value === null ? metadata[12 + index] : +value))
    ];
  }

  return { model, uid: card.uid, memory: memory.flat(), metadata, secrets };
}
