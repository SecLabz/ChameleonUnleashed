import { isByte } from '../documents/slot-image.mjs';
import { COMMAND } from './commands.mjs';
import { ST25TB, st25tbIdentity } from './st25tb-protocol.mjs';
import { hfDocument, HF_NOTES } from '../documents/hf-image.mjs';

const knownBlock = (bytes) => bytes?.length === 4 && bytes.every(isByte);

// Active NFC discovery and one-command captures. The caller owns reservation/end.
const unsupportedCommand = (error) => error.httpStatus === 404 || [0x67, 0x69].includes(error.status);

export function crcOk(bytes) {
  if (bytes.length < 3) {
    return false;
  }

  let crc = 0xffff;
  for (const byte of bytes.slice(0, -2)) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0x8408 : 0);
    }
  }

  crc ^= 0xffff;
  return bytes.at(-2) === (crc & 255) && bytes.at(-1) === crc >>> 8;
}

function systemInfo(data, card) {
  if (
    data.length < 12 ||
    data[0] ||
    data[1] & 0xf0 ||
    !crcOk(data) ||
    [...data.slice(2, 10)].reverse().join() !== card.uid.join()
  ) {
    throw new Error('Invalid NFC-V system information');
  }

  let offset = 10;
  for (let field = 0; field < 4; field++) {
    if (!(data[1] & (1 << field))) {
      continue;
    }

    const count = field === 2 ? 2 : 1;
    if (offset + count > data.length - 2) {
      throw new Error('Truncated NFC-V system information');
    }

    if (field === 0) {
      card.dsfid = data[offset];
    }
    if (field === 1) {
      card.afi = data[offset];
    }
    if (field === 2) {
      card.blocks = data[offset] + 1;
      card.block_size = (data[offset + 1] & 31) + 1;
    }
    if (field === 3) {
      card.ic_ref = data[offset];
    }
    offset += count;
  }

  if (offset !== data.length - 2) {
    throw new Error('Trailing NFC-V system information');
  }
}

export async function scanNfcA(device, polling = false) {
  const data = await device.command(COMMAND.SCAN_A, polling ? [1] : []);
  const uidLength = data[0];
  if (
    ![4, 7, 10].includes(uidLength) ||
    data.length < uidLength + 5 ||
    data.length !== uidLength + 5 + data[uidLength + 4]
  ) {
    throw new Error('Invalid card identity');
  }
  return {
    uid: Array.from(data.slice(1, uidLength + 1)),
    atqa: Array.from(data.slice(uidLength + 1, uidLength + 3)).reverse(),
    sak: data[uidLength + 3],
    ats: Array.from(data.slice(uidLength + 5))
  };
}

export async function scanNfcV(device, password) {
  const data = await device.command(
    password ? COMMAND.SLIX_PRIVACY_SCAN : COMMAND.NFCV_INVENTORY,
    password || []
  );
  const card = await nfcvIdentity(device, data);
  if (password && card.uid.slice(0, 3).join() !== '224,4,3') {
    throw new Error('The unlocked tag is not an ICODE SLIX-L.');
  }
  return card;
}

async function nfcvIdentity(device, data) {
  if (data.length !== 12 || data[0] || !crcOk(data)) {
    throw new Error('Invalid NFC-V Inventory');
  }

  const card = {
    protocol: 'nfc_v',
    uid: Array.from(data.slice(2, 10)).reverse(),
    dsfid: data[1],
    afi: null,
    ic_ref: null,
    blocks: null,
    block_size: null
  };
  try {
    systemInfo(await device.command(COMMAND.NFCV_SYSTEM_INFO, card.uid), card);
  } catch (error) {
    const systemInfoUnavailable =
      unsupportedCommand(error) ||
      (error.status === 2 &&
        error.data?.length === 4 &&
        error.data[0] & 1 &&
        error.data[1] === 1 &&
        crcOk(error.data));
    if (!systemInfoUnavailable) {
      throw error;
    }
  }
  return card;
}

export async function scanNfc(device, onFamily = () => {}, polling = false) {
  onFamily('NFC-A');
  try {
    return await scanNfcA(device, polling);
  } catch (error) {
    if (error.status !== 1) {
      throw error;
    }
  }

  let available = false;
  for (const [command, label] of [
    [COMMAND.SCAN_B, 'NFC-B'],
    [ST25TB.INFO, 'ST25TB'],
    [COMMAND.NFCV_INVENTORY, 'NFC-V']
  ]) {
    onFamily(label);
    let data;
    try {
      data = await device.command(command);
      available = true;
    } catch (error) {
      if (unsupportedCommand(error)) {
        continue;
      }
      if (error.status === 1) {
        available = true;
        continue;
      }
      throw error;
    }

    if (command === COMMAND.SCAN_B) {
      if (data.length !== 14 || data[0] !== 0x50 || !crcOk(data)) {
        throw new Error('Invalid ATQB response');
      }
      return {
        protocol: 'nfc_b',
        pupi: Array.from(data.slice(1, 5)),
        application: Array.from(data.slice(5, 9)),
        protocol_info: Array.from(data.slice(9, 12))
      };
    }

    if (command === ST25TB.INFO) {
      const card = st25tbIdentity(data);
      if (!card) {
        throw new Error('Invalid ST25TB identity');
      }
      return card;
    }

    return await nfcvIdentity(device, data);
  }

  const error = new Error(
    available
      ? 'No supported NFC tag found'
      : 'No NFC-A tag found; update firmware for NFC-B and NFC-V readers'
  );
  error.status = 1;
  throw error;
}

export function decodeSnapshot(data, card) {
  const blocks = (data[12] << 8) | data[13];
  const family = card.protocol === 'st25tb' ? 2 : 3;
  if (
    data.length < 24 ||
    data[0] !== 1 ||
    data[1] !== family ||
    data[2] > 2 ||
    data[14] !== 4 ||
    data[15] & ~15 ||
    !blocks ||
    blocks > 64 ||
    blocks !== card.blocks ||
    data.length !== 24 + blocks * 6 ||
    Array.from(data.slice(4, 12)).join() !== card.uid.join() ||
    (family === 2 && data[15] & ~8) ||
    (family === 3 && data[15] & 8) ||
    (data[2] === 0 && data[3])
  ) {
    throw new Error('Invalid reader snapshot');
  }

  const memory = [];
  const locks = [];
  for (let index = 0; index < blocks; index++) {
    const offset = 24 + index * 6;
    if (data[offset] > 1 || data[offset + 1] > 2) {
      throw new Error('Invalid reader block state');
    }
    memory.push(data[offset] ? Array.from(data.slice(offset + 2, offset + 6)) : null);
    locks.push(data[offset + 1] === 0 ? null : data[offset + 1] === 2);
  }

  const system = data[15] & 8 ? Array.from(data.slice(20, 24)) : null;
  const complete = data[2] === 0;
  if (complete && (memory.some((block) => !knownBlock(block)) || (family === 2 && !system))) {
    throw new Error('Incomplete data marked complete');
  }

  return {
    version: 1,
    protocol: card.protocol,
    uid: [...card.uid],
    memory,
    locks,
    dsfid: data[15] & 1 ? data[16] : (card.dsfid ?? null),
    afi: data[15] & 2 ? data[17] : (card.afi ?? null),
    ic_ref: data[15] & 4 ? data[18] : (card.ic_ref ?? null),
    chip_id: family === 2 ? data[19] : null,
    system,
    complete,
    deadline: data[2] === 2,
    status: data[3],
    tag_error: family === 3 && data[19] ? data[19] : null
  };
}

export async function readHf(device, document, onUpdate) {
  if (document.unbound) {
    const next = hfDocument(await scanNfcV(device, device.nfcvPassword));
    if (next.type !== document.type) {
      throw new Error(`The presented tag is not an ICODE ${document.geometry.name}.`);
    }
    next.name = document.name;
    document = next;
  }

  const { card } = document;
  if (!document.memory.length) {
    throw new Error('Memory geometry is unsupported');
  }

  const payload = [...card.uid];
  if (card.protocol === 'nfc_v') {
    payload.push(card.blocks >> 8, card.blocks & 255, 4);
  }

  onUpdate(document, 'Reading tag; Stop waits for this bounded dump…');
  const reply = await device.dump(card.protocol === 'st25tb' ? ST25TB.DUMP : COMMAND.NFCV_DUMP, payload);
  if (reply.data.length < 24) {
    throw new Error(`Reader status ${reply.status}`);
  }

  const snapshot = decodeSnapshot(reply.data, card);
  if (reply.status !== 0) {
    snapshot.complete = false;
    snapshot.status = reply.status;
  }

  document.capture = snapshot;
  if (card.protocol === 'st25tb') {
    card.chip_id = snapshot.chip_id;
    card.system = snapshot.system;
  }
  if (card.protocol === 'nfc_v') {
    card.dsfid = snapshot.dsfid;
    card.afi = snapshot.afi;
    card.ic_ref = snapshot.ic_ref;
  }

  document.memory = snapshot.memory.map((block) => (block ? [...block] : null));
  document.locks = [...snapshot.locks];
  document.warnings = [];
  if (!snapshot.complete) {
    document.warnings.push(
      snapshot.deadline
        ? 'Read deadline reached; acquired blocks retained'
        : 'Read incomplete; acquired blocks retained'
    );
  }
  if (card.protocol === 'nfc_v') {
    document.warnings.push(HF_NOTES.credentials);
  }

  let message = snapshot.complete ? 'Read complete' : document.warnings[0];
  if (device.signal?.aborted) {
    message = 'Read stopped; returned blocks retained';
  }
  onUpdate(document, message);
  return document;
}
