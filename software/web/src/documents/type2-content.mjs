import { bytesValid } from './slot-image.mjs';

const URI_PREFIXES = [
  '',
  'http://www.',
  'https://www.',
  'http://',
  'https://',
  'tel:',
  'mailto:',
  'ftp://anonymous:anonymous@',
  'ftp://ftp.',
  'ftps://',
  'sftp://',
  'smb://',
  'nfs://',
  'ftp://',
  'dav://',
  'news:',
  'telnet://',
  'imap:',
  'rtsp://',
  'urn:',
  'pop:',
  'sip:',
  'sips:',
  'tftp:',
  'btspp://',
  'btl2cap://',
  'btgoep://',
  'tcpobex://',
  'irdaobex://',
  'file://',
  'urn:epc:id:',
  'urn:epc:tag:',
  'urn:epc:pat:',
  'urn:epc:raw:',
  'urn:epc:',
  'urn:nfc:'
];

export function type2UserEnd(document) {
  return (
    {
      ntag213: 40,
      ntag215: 130,
      ntag216: 226,
      ultralight: 16,
      ultralight_c: 40,
      ultralight_ev1_48: 16,
      ultralight_ev1_128: 36,
      ntag210: 16,
      ntag212: 36
    }[document?.geometry?.profile] || 0
  );
}

export function amiiboInfo(document) {
  if (document?.type !== 1101 || document.memory.length !== 135) {
    return null;
  }
  const page = (index) => document.memory[index];
  const known = (index) => bytesValid(page(index)) && page(index).length === 4;
  if (
    ![0, 3, 4, 131, 132].every(known) ||
    page(0)[0] !== 4 ||
    page(3).join() !== '241,16,255,238' ||
    page(4)[0] !== 0xa5 ||
    page(131)[3] !== 4 ||
    page(132)[0] !== 0x5f
  ) {
    return null;
  }
  const id = [21, 22].every(known) ? [...page(21), ...page(22)] : null;
  return {
    id,
    complete: document.memory.slice(0, 133).every((bytes) => bytesValid(bytes) && bytes.length === 4)
  };
}

function records(bytes) {
  const result = [];
  let offset = 0;
  let ended = false;
  while (offset < bytes.length) {
    const header = bytes[offset++];
    if (ended || !!(header & 0x80) !== (result.length === 0) || header & 0x20 || (header & 7) === 7) {
      throw new Error('Unsupported or malformed NDEF record sequence');
    }
    const take = (count) => {
      if (offset + count > bytes.length) {
        throw new Error('Truncated NDEF record');
      }
      const part = bytes.slice(offset, offset + count);
      offset += count;
      return part;
    };
    const typeLength = take(1)[0];
    const sizeBytes = take(header & 0x10 ? 1 : 4);
    const size = sizeBytes.reduce((value, byte) => value * 256 + byte, 0);
    const idSize = header & 8 ? take(1)[0] : 0;
    const type = take(typeLength);
    take(idSize);
    const payload = take(size);
    let record = { kind: 'raw', value: '', bytes: payload.length };
    const decoder = new TextDecoder('utf-8', { fatal: true });
    if (
      (header & 7) === 1 &&
      type.length === 1 &&
      type[0] === 0x54 &&
      payload.length &&
      !(payload[0] & 0xc0)
    ) {
      const languageSize = payload[0] & 63;
      if (1 + languageSize > payload.length) {
        throw new Error('Truncated NDEF text language');
      }
      record = {
        language: decoder.decode(new Uint8Array(payload.slice(1, 1 + languageSize))),
        kind: 'text',
        value: decoder.decode(new Uint8Array(payload.slice(1 + languageSize))),
        bytes: payload.length
      };
    } else if (
      (header & 7) === 1 &&
      type.length === 1 &&
      type[0] === 0x55 &&
      payload.length &&
      payload[0] < URI_PREFIXES.length
    ) {
      record = {
        kind: 'uri',
        value: URI_PREFIXES[payload[0]] + decoder.decode(new Uint8Array(payload.slice(1))),
        bytes: payload.length
      };
    }
    result.push({ ...record, editable: idSize === 0 });
    ended = !!(header & 0x40);
  }
  if (bytes.length && !ended) {
    throw new Error('NDEF final record is missing');
  }
  return result;
}

export function inspectNdef(document) {
  const unavailable = (message) => ({ editable: false, records: [], message });
  if (document?.kind !== 'type2') {
    return unavailable('');
  }
  const cc = document.memory[3];
  if (!bytesValid(cc) || cc.length !== 4) {
    return unavailable('Read the capability page to inspect NDEF.');
  }
  if (cc[0] !== 0xe1 || cc[1] >> 4 !== 1 || !cc[2]) {
    return unavailable('This tag is not NFC Forum formatted. Its memory is preserved.');
  }
  const end = 16 + cc[2] * 8;
  if (end > type2UserEnd(document) * 4) {
    return unavailable('The declared NDEF capacity exceeds the user memory.');
  }
  const byte = (index) => {
    const page = document.memory[Math.floor(index / 4)];
    if (!bytesValid(page) || page.length !== 4 || index >= end) {
      throw new Error('NDEF contains unread bytes. Read the tag before editing its content.');
    }
    return page[index % 4];
  };
  let found = null;
  try {
    let offset = 16;
    while (offset < end) {
      const start = offset;
      const type = byte(offset++);
      if (!type) {
        continue;
      }
      if (type === 0xfe) {
        break;
      }
      let size = byte(offset++);
      if (size === 0xff) {
        size = byte(offset) * 256 + byte(offset + 1);
        offset += 2;
      }
      if (offset + size > end) {
        throw new Error('A TLV exceeds the declared NDEF capacity.');
      }
      if (type === 1) {
        if (size !== 3) {
          throw new Error('Invalid NDEF lock-control layout.');
        }
        const position = byte(offset);
        const lockBytes = Math.ceil((byte(offset + 1) || 256) / 8);
        const address = (position >> 4) * 2 ** (byte(offset + 2) & 15) + (position & 15);
        if ((address < end && address + lockBytes > 16) || address + lockBytes > document.memory.length * 4) {
          return unavailable('Reserved lock-byte layouts are preserved; use the memory view.');
        }
      }
      if (type === 2) {
        return unavailable('Reserved-memory NDEF layouts are preserved; use the memory view.');
      }
      if (found) {
        found.editable = false;
        found.message = 'Other TLVs follow this message. Its content is preserved.';
      }
      if (type === 3) {
        const entries = records(Array.from({ length: size }, (_, index) => byte(offset + index)));
        const editable =
          !found &&
          (entries.length === 0 ||
            (entries.length === 1 && ['text', 'uri'].includes(entries[0].kind) && entries[0].editable));
        if (found) {
          found.records.push(...entries);
        } else {
          found = {
            start,
            end,
            next: offset + size,
            records: entries,
            editable,
            message: editable
              ? ''
              : 'Complex NDEF message. Its records are preserved; editing is available for one text or URL record.'
          };
        }
      }
      offset += size;
    }
    if (!found) {
      return unavailable('No NDEF message found. Its memory is preserved.');
    }
    // Expanding a message must never overwrite unread or unrelated trailing data.
    let capacityEnd = found.next;
    for (; capacityEnd < end; capacityEnd++) {
      const value = byte(capacityEnd);
      if (value !== 0 && !(capacityEnd === found.next && value === 0xfe)) {
        break;
      }
    }
    found.end = capacityEnd;
    return found;
  } catch (error) {
    if (found) {
      return { ...found, editable: false, message: error.message };
    }
    return unavailable(error.message);
  }
}

export function applyNdef(document, kind, value) {
  const info = inspectNdef(document);
  if (!info.editable || !['text', 'uri'].includes(kind)) {
    throw new Error(info.message || 'This NDEF message cannot be edited.');
  }
  if (typeof value !== 'string' || value.length > info.end - info.start) {
    throw new Error(`Content exceeds the ${info.end - info.start} available bytes.`);
  }
  const bytes = Array.from(new TextEncoder().encode(value));
  let payload;
  if (kind === 'text') {
    const language = new TextEncoder().encode(info.records[0]?.language || 'en');
    payload = [language.length, ...language, ...bytes];
  } else {
    try {
      new URL(value);
    } catch {
      throw new Error('Enter a full URL, such as https://example.com.');
    }
    const prefix = URI_PREFIXES.map((text, index) => ({ text, index }))
      .filter((item) => value.startsWith(item.text))
      .sort((left, right) => right.text.length - left.text.length)[0];
    payload = [prefix.index, ...new TextEncoder().encode(value.slice(prefix.text.length))];
  }
  const short = payload.length < 256;
  const size = short
    ? [payload.length]
    : [
        payload.length >>> 24,
        (payload.length >>> 16) & 255,
        (payload.length >>> 8) & 255,
        payload.length & 255
      ];
  const header = short ? 0xd1 : 0xc1;
  const type = kind === 'text' ? 0x54 : 0x55;
  const message = [header, 1, ...size, type, ...payload];
  const length = message.length < 255 ? [message.length] : [0xff, message.length >>> 8, message.length & 255];
  const encoded = [3, ...length, ...message];
  const available = info.end - info.start;
  if (encoded.length > available) {
    throw new Error(`Content needs ${encoded.length} bytes; ${available} bytes are available.`);
  }
  if (encoded.length < available) {
    encoded.push(0xfe);
  }
  // Only replace this TLV and its padding. Keep all other pages unchanged.
  const memory = document.memory.map((page) => page && [...page]);
  const changedEnd = Math.max(info.next + 1, info.start + encoded.length);
  for (let offset = info.start; offset < Math.min(changedEnd, info.end); offset++) {
    memory[Math.floor(offset / 4)][offset % 4] = encoded[offset - info.start] ?? 0;
  }
  return memory;
}

export function type2Changes(document) {
  const reading = document?.type2Read;
  if (document?.kind !== 'type2' || !reading || document.type !== reading.type) {
    return null;
  }
  const writes = [];
  const unknown = [];
  const userEnd = type2UserEnd(document);
  for (let index = 0; index < document.memory.length; index++) {
    if (index < 4 || index >= userEnd) {
      continue;
    }

    const before = reading.memory[index];
    const after = document.memory[index];
    if (JSON.stringify(before) === JSON.stringify(after)) {
      continue;
    }
    if (!bytesValid(before) || !bytesValid(after) || before.length !== 4 || after.length !== 4) {
      unknown.push(index);
    } else {
      writes.push({ index, before: [...before], after: [...after] });
    }
  }
  return { writes, unknown };
}
