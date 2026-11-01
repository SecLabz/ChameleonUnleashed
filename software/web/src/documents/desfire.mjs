// Public DESFire observations. Storage is proportional to captured data, never
// the advertised card capacity. The current HF slot budget remains 4500 bytes.
import { hex } from '../device/transport.mjs';

export const DESFIRE_STATES = ['Read', 'Access denied', 'Unsupported', 'Not read'];

const bytes = (value) =>
  Array.isArray(value) && value.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 255);

const be16 = (value) => [value >> 8, value & 255];

export const readLittleEndian = (bytes) =>
  bytes.reduce((value, byte, index) => value + byte * 2 ** (8 * index), 0);

const hasSignature = (bytes, signature) =>
  bytes.slice(0, 4).every((byte, index) => byte === signature.charCodeAt(index));

const lengthPrefixed = (bytes) => [bytes.length, ...bytes];

export const littleEndianBytes = (value, width) =>
  Array.from({ length: width }, (_, index) => Math.floor(value / 2 ** (8 * index)) & 255);

export const allowsPublicRead = (settings) =>
  settings.length >= 4 && settings[1] === 0 && (settings[2] >> 4 === 14 || settings[3] >> 4 === 14);

export function desfireLayout(settings) {
  if (settings.length < 4) {
    throw new Error('Truncated DESFire settings');
  }

  if (settings[0] <= 1 && settings.length >= 7) {
    return [0xbd, readLittleEndian(settings.slice(4, 7)), 1];
  }
  if (settings[0] === 2 && settings.length >= 17) {
    return [0x6c, 4, 1];
  }

  if ([3, 4].includes(settings[0]) && settings.length >= 13) {
    const width = readLittleEndian(settings.slice(4, 7));
    const count = readLittleEndian(settings.slice(10, 13));
    if (!width || count > readLittleEndian(settings.slice(7, 10))) {
      throw new Error('Invalid DESFire records');
    }
    return [0xbb, width * count, width];
  }

  if (settings[0] <= 4) {
    throw new Error('Invalid DESFire geometry');
  }
  return null;
}

function captureReader(buffer) {
  let offset = 0;

  const take = (length) => {
    if (!Number.isInteger(length) || length < 0 || offset + length > buffer.length) {
      throw new Error('Truncated DESFire capture');
    }
    const value = buffer.slice(offset, offset + length);
    offset += length;
    return value;
  };

  const number = (length) => readLittleEndian(take(length));

  const state = () => {
    const value = number(1);
    if (value > 3) {
      throw new Error('Invalid DESFire state');
    }
    return value;
  };

  const keys = () => {
    const status = state();
    const settings = take(number(1));
    const versions = take(number(1));
    const invalidObservation =
      status === 0
        ? settings.length !== 2 || versions.length > (settings[1] & 15)
        : settings.length !== 0 || versions.length !== 0;

    if (settings.length > 2 || versions.length > 14 || invalidObservation) {
      throw new Error('Invalid DESFire key observations');
    }
    return { status, settings, versions };
  };

  return {
    take,
    number,
    state,
    keys,
    finish() {
      if (offset !== buffer.length) {
        throw new Error('Trailing DESFire bytes');
      }
    }
  };
}

export function inspectDesfire(snapshot) {
  if (
    !snapshot ||
    snapshot.version !== 1 ||
    snapshot.protocol !== 'desfire' ||
    !bytes(snapshot.uid) ||
    ![4, 7, 10].includes(snapshot.uid.length) ||
    !bytes(snapshot.metadata) ||
    snapshot.metadata.length > 96 ||
    !bytes(snapshot.memory) ||
    snapshot.memory.length > 8192 ||
    typeof snapshot.complete !== 'boolean' ||
    !(snapshot.error === null || typeof snapshot.error === 'string') ||
    (snapshot.complete && snapshot.error)
  ) {
    throw new Error('Invalid DESFire capture');
  }

  const full = hasSignature(snapshot.metadata, 'DFA1');
  const version = snapshot.metadata[4];
  if (full ? version !== 1 : !hasSignature(snapshot.metadata, 'DFU1') || snapshot.memory.length) {
    throw new Error('Unsupported DESFire capture');
  }

  const metadataReader = captureReader(snapshot.metadata.slice(full ? 5 : 4));
  const atqa = metadataReader.take(2);
  const sak = metadataReader.number(1);
  const ats = metadataReader.take(metadataReader.number(1));
  if (
    !(sak & 32) ||
    ats.length < 2 ||
    ats.length > 32 ||
    ats[0] !== ats.length ||
    (ats[1] & 15) > 8 ||
    (atqa[0] & 0xc0) !== { 4: 0, 7: 0x40, 10: 0x80 }[snapshot.uid.length]
  ) {
    throw new Error('Invalid DESFire activation');
  }

  const result = {
    card: { protocol: 'desfire', uid: [...snapshot.uid], atqa: [...atqa].reverse(), sak, ats },
    version: [],
    applications: [],
    full
  };
  if (!full) {
    metadataReader.finish();
    return result;
  }

  result.version = metadataReader.take(28);
  const freeMemoryPresent = metadataReader.number(1);
  const freeMemory = metadataReader.number(4);
  if (freeMemoryPresent > 1 || freeMemory > 0xffffff) {
    throw new Error('Invalid DESFire free memory');
  }
  result.free_memory = freeMemoryPresent ? freeMemory : null;
  result.keys = metadataReader.keys();
  result.applications_state = metadataReader.state();
  metadataReader.finish();

  if (!hasSignature(snapshot.memory, 'DFD1') || snapshot.memory[4] !== version) {
    throw new Error('Invalid DESFire file data');
  }
  const dataReader = captureReader(snapshot.memory.slice(5));
  const applicationCount = dataReader.number(1);
  if (applicationCount > 28 || (result.applications_state && applicationCount)) {
    throw new Error('Invalid DESFire directory');
  }

  let totalFiles = 0;
  const applicationIds = new Set();
  for (let applicationIndex = 0; applicationIndex < applicationCount; applicationIndex++) {
    const aid = dataReader.take(3);
    const applicationId = hex(aid);
    if (!aid.some(Boolean) || applicationIds.has(applicationId)) {
      throw new Error('Invalid DESFire AID');
    }
    applicationIds.add(applicationId);

    const application = {
      aid,
      keys: dataReader.keys(),
      files_state: dataReader.state(),
      files: []
    };
    const fileCount = dataReader.number(1);
    totalFiles += fileCount;
    if (fileCount > 32 || totalFiles > 64 || (application.files_state && fileCount)) {
      throw new Error('Invalid DESFire files');
    }

    const fileIds = new Set();
    for (let fileIndex = 0; fileIndex < fileCount; fileIndex++) {
      const id = dataReader.number(1);
      const settings_state = dataReader.state();
      const settings = dataReader.take(dataReader.number(2));
      const data_state = dataReader.state();
      const data = dataReader.take(dataReader.number(4));
      const invalidSettings = settings_state
        ? settings.length !== 0 || data_state === 0
        : settings.length < 4;

      if (
        id > 31 ||
        fileIds.has(id) ||
        settings.length > 96 ||
        (data_state && data.length) ||
        invalidSettings
      ) {
        throw new Error('Invalid DESFire file');
      }
      fileIds.add(id);

      const fileLayout = settings_state === 0 ? desfireLayout(settings) : null;
      if (data_state === 0 && (!allowsPublicRead(settings) || !fileLayout || fileLayout[1] !== data.length)) {
        throw new Error('Invalid public DESFire data');
      }
      application.files.push({ id, settings_state, settings, data_state, data });
    }
    result.applications.push(application);
  }

  dataReader.finish();
  return result;
}

export function desfireDocument(snapshot, name = 'DESFire') {
  const details = inspectDesfire(snapshot);
  const generation = { 0x12: ' EV2', 1: ' EV1', 0x33: ' EV3' }[details.version[3]] || '';

  return {
    name,
    type: 1300,
    kind: 'desfire',
    unitBytes: 1,
    card: details.card,
    memory: [],
    capture: snapshot,
    geometry: { name: `MIFARE DESFire${generation}` },
    warnings: [
      'Public data emulation; authentication and secure messaging are unavailable',
      ...(snapshot.error ? [snapshot.error] : [])
    ]
  };
}

export function encodeDesfireSlot(document) {
  const capture = document.capture;
  inspectDesfire(capture);
  const raw = [
    capture.uid.length,
    ...capture.uid,
    ...be16(capture.metadata.length),
    ...be16(capture.memory.length),
    ...capture.metadata,
    ...capture.memory
  ];
  while (raw.length % 4) {
    raw.push(0);
  }
  // Same limit as firmware; reject before starting an upload, never truncate.
  if (raw.length > 4492) {
    throw new Error('DESFire image exceeds the 4500-byte slot capacity; export JSON to keep it');
  }
  return new Uint8Array([2, 5, 20, ...be16(raw.length), ...raw]);
}

export function decodeDesfireSlot(wire, name) {
  const raw = Array.from(wire.slice(5));
  const uidLength = raw[0];
  const metadataLength = (raw[uidLength + 1] << 8) | raw[uidLength + 2];
  const memoryLength = (raw[uidLength + 3] << 8) | raw[uidLength + 4];
  const end = uidLength + 5 + metadataLength + memoryLength;
  if (
    wire.length > 4500 ||
    wire.length !== 5 + ((wire[3] << 8) | wire[4]) ||
    raw.length !== ((end + 3) & ~3) ||
    raw.slice(end).some(Boolean)
  ) {
    throw new Error('Invalid DESFire slot size');
  }
  return desfireDocument(
    {
      version: 1,
      protocol: 'desfire',
      uid: raw.slice(1, uidLength + 1),
      metadata: raw.slice(uidLength + 5, uidLength + 5 + metadataLength),
      memory: raw.slice(uidLength + 5 + metadataLength, end),
      complete: true,
      error: null
    },
    name
  );
}

export function desfireImage(document) {
  const capture = document.capture;
  const details = inspectDesfire(capture);
  return {
    model: details.full ? 18 : 14,
    uid: capture.uid,
    memory: capture.memory,
    metadata: capture.metadata,
    secrets: []
  };
}

export function importDesfireImage(image, name) {
  if (![14, 18].includes(image.model) || image.secrets.length) {
    throw new Error('Invalid DESFire image');
  }
  return desfireDocument(
    {
      version: 1,
      protocol: 'desfire',
      uid: image.uid,
      memory: image.memory,
      metadata: image.metadata,
      complete: true,
      error: null
    },
    name
  );
}

export function createSnapshot(capture, complete = false, error = null) {
  const metadata = [
    68,
    70,
    65,
    49,
    1,
    ...capture.atqa,
    capture.sak,
    ...lengthPrefixed(capture.ats),
    ...capture.version,
    +(capture.free_memory !== null),
    ...littleEndianBytes(capture.free_memory || 0, 4),
    capture.keys.status,
    ...lengthPrefixed(capture.keys.settings),
    ...lengthPrefixed(capture.keys.versions),
    capture.applications_state
  ];
  const memory = [68, 70, 68, 49, 1, capture.applications.length];

  for (const application of capture.applications) {
    memory.push(
      ...application.aid,
      application.keys.status,
      ...lengthPrefixed(application.keys.settings),
      ...lengthPrefixed(application.keys.versions),
      application.files_state,
      application.files.length
    );

    for (const file of application.files) {
      memory.push(
        file.id,
        file.settings_state,
        ...littleEndianBytes(file.settings.length, 2),
        ...file.settings,
        file.data_state,
        ...littleEndianBytes(file.data.length, 4),
        ...file.data
      );
    }
  }

  const result = { version: 1, protocol: 'desfire', uid: capture.uid, metadata, memory, complete, error };
  inspectDesfire(result);
  return result;
}
