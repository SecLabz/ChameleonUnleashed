import { hex } from '../device/transport.mjs';
import { HID_FORMATS } from './hid-formats.mjs';
import { LF_RECORD_BYTES, typeName } from './slot-image.mjs';
import { parseHex } from './workspace.mjs';

export { HID_FORMATS };

export const LF_TYPES = [100, 170, 200, 201, 150, 180, 310, 104];

export function blankLf(type = 100) {
  if (!LF_RECORD_BYTES[type]) {
    throw new Error('Unsupported RFID format');
  }
  return {
    name: `${typeName(type)} draft`,
    type,
    kind: 'lf',
    unitBytes: LF_RECORD_BYTES[type],
    card: { uid: [], atqa: [], sak: 0, ats: [] },
    memory: [null],
    lf: {}
  };
}

const readUnsigned = (bytes) => bytes.reduce((value, byte) => value * 256 + byte, 0);
const bigEndian = (value, size) =>
  Array.from({ length: size }, (_, index) => Math.floor(value / 2 ** (8 * (size - index - 1))) & 255);

export function lfFields(doc) {
  if (doc.lf?.input) {
    return { ...doc.lf.input };
  }
  const data = doc.memory[0];
  if (doc.type === 200) {
    return {
      format: String(data?.[0] ?? 1),
      facility: data ? String(readUnsigned(data.slice(1, 5))) : '',
      number: data ? String(readUnsigned(data.slice(5, 10))) : '',
      issue: String(data?.[10] ?? 0),
      oem: data ? String(readUnsigned(data.slice(11, 13))) : '900',
      raw: data ? hex(data) : ''
    };
  }
  if (doc.type === 201) {
    return {
      version: String(data?.[0] ?? 1),
      facility: data ? String(data[1]) : '',
      number: data ? String(readUnsigned(data.slice(2, 4))) : ''
    };
  }
  if (doc.type === 150) {
    return { id: data ? String.fromCharCode(...data) : '' };
  }
  if (doc.type === 104) {
    return { id: data ? hex(data.slice(0, 5)) : '', epilogue: data ? hex(data.slice(5)) : '' };
  }
  return { id: data ? hex(doc.type === 310 ? data.slice(4) : data) : '' };
}

export function lfSchema(type, fields) {
  const decimal = (key, label, limit) => ({ key, label, limit, help: `Decimal · 0–${limit}` });
  const hexadecimal = (key, label, bytes) => ({
    key,
    label,
    bytes,
    help: `${bytes * 2} hexadecimal characters · leading zeros are kept`
  });
  if (type === 200) {
    const format = HID_FORMATS.find((entry) => entry[1] === Number(fields.format));
    if (!format) {
      return [hexadecimal('raw', 'Raw HID record', 13)];
    }
    return [
      decimal('facility', 'Facility code', format[2][0]),
      decimal('number', 'Card number', format[2][1]),
      decimal('issue', 'Issue level', format[2][2]),
      decimal('oem', 'OEM code', format[2][3])
    ].filter((field) => field.limit > 0);
  }
  if (type === 201) {
    return [
      decimal('version', 'Version', 255),
      decimal('facility', 'Facility code', 255),
      decimal('number', 'Card number', 65535)
    ];
  }
  if (type === 150) {
    return [{ key: 'id', label: 'RFID ID', help: '8 printable ASCII characters · no spaces' }];
  }
  if (type === 104) {
    return [hexadecimal('id', 'RFID ID', 5), hexadecimal('epilogue', 'Electra additional data', 8)];
  }
  return [hexadecimal('id', 'RFID ID', type === 310 ? 4 : LF_RECORD_BYTES[type])];
}

export function encodeLfFields(type, fields) {
  const parsed = {};
  for (const field of lfSchema(type, fields)) {
    const value = fields[field.key] ?? '';
    try {
      if (field.limit !== undefined) {
        if (!/^\d+$/.test(value) || Number(value) > field.limit) {
          throw new Error(`Enter a decimal number from 0 to ${field.limit}`);
        }
        parsed[field.key] = Number(value);
      } else if (field.bytes) {
        parsed[field.key] = parseHex(value, field.bytes);
      } else {
        if (!/^[\x21-\x7e]{8}$/.test(value)) {
          throw new Error('Enter exactly 8 printable ASCII characters, without spaces');
        }
        parsed[field.key] = Array.from(value, (character) => character.charCodeAt(0));
      }
    } catch (error) {
      throw Object.assign(error, { field: field.key });
    }
  }
  if (type === 200) {
    const format = HID_FORMATS.find((entry) => entry[1] === Number(fields.format));
    if (!format) {
      return parsed.raw;
    }
    let oem = parsed.oem ?? 0;
    if ([22, 42].includes(format[1]) && !oem) {
      oem = 900;
    }
    return [
      format[1],
      ...bigEndian(parsed.facility ?? 0, 4),
      ...bigEndian(parsed.number, 5),
      parsed.issue ?? 0,
      ...bigEndian(oem, 2)
    ];
  }
  if (type === 201) {
    const { version, facility, number: cardNumber } = parsed;
    const [high, low] = bigEndian(cardNumber, 2);
    const checksum = 255 - ((0xf0 + facility + version + high + low) & 255);
    // Firmware ioProx uses seven 8+1 groups and a final stop bit, MSB first.
    const bits = [0, 0xf0, facility, version, high, low, checksum].flatMap((byte, index) => [
      ...Array.from({ length: 8 }, (_, bit) => (byte >> (7 - bit)) & 1),
      index === 0 ? 0 : 1
    ]);
    bits.push(1);
    const raw = Array.from({ length: 8 }, (_, index) =>
      bits.slice(index * 8, index * 8 + 8).reduce((byte, bit) => byte * 2 + bit, 0)
    );
    return [version, facility, high, low, ...raw, 0, 0, 0, 0];
  }
  if (type === 104) {
    return [...parsed.id, ...parsed.epilogue];
  }
  if (type === 310) {
    return [0x49, 0x44, 0x54, 0x4b, ...parsed.id];
  }
  return parsed.id;
}

export function editLf(doc, fields) {
  doc.lf ??= {};
  try {
    doc.memory[0] = encodeLfFields(doc.type, fields);
    delete doc.lf.input;
    return null;
  } catch (error) {
    // Invalid text survives reload, while no previous valid ID can be sent by accident.
    doc.memory[0] = null;
    doc.lf.input = { ...fields };
    return error;
  }
}

export function lfIdentity(doc) {
  if (!doc.memory.length) {
    return 'No decoded ID';
  }
  if (!doc.memory[0]) {
    return 'ID not set';
  }
  const fields = lfFields(doc);
  if (doc.type === 200) {
    const format = HID_FORMATS.find((entry) => entry[1] === Number(fields.format));
    if (!format) {
      return `Record ${fields.raw}`;
    }
    if (!format[2][0]) {
      return `Card ${fields.number}`;
    }
  }
  if (doc.type === 200 || doc.type === 201) {
    return `FC ${fields.facility} · Card ${fields.number}`;
  }
  return `ID ${fields.id}`;
}
