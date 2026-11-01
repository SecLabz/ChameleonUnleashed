import { hex } from './transport.mjs';
import { typeName } from '../documents/slot-image.mjs';
import { inspectLfChip } from './lf-program.mjs';

const PROTOCOLS = [
  { command: 3000, type: 100, size: 5, payload: [] },
  { command: 3004, type: 170, size: 4, payload: [] },
  { command: 3002, type: 200, size: 13, payload: [0] },
  { command: 3010, type: 201, size: 16, payload: [0] },
  { command: 3014, type: 150, size: 8, payload: [] },
  { command: 3019, type: 180, size: 5, payload: [] }
];

export async function scanLf(device, onFamily = () => {}) {
  for (const protocol of PROTOCOLS) {
    onFamily(typeName(protocol.type));
    let bytes;
    try {
      bytes = await device.command(protocol.command, protocol.payload);
    } catch (error) {
      if (error.status === 0x41) {
        continue;
      }
      throw error;
    }

    let { type, size } = protocol;
    const warnings = [];
    let clock;
    let label = typeName(type);

    if (protocol.command === 3000) {
      type = bytes[0] * 256 + bytes[1];
      if (![100, 101, 102, 103, 104].includes(type)) {
        throw new Error('Invalid EM410x scan type');
      }

      size = type === 104 ? 13 : 5;
      if (type >= 101 && type <= 103) {
        const rate = { 101: 16, 102: 32, 103: 64 }[type];
        clock = rate;
        label = `EM410x/${rate}`;
        if (rate !== 64) {
          warnings.push(`Detected clock/${rate}; Chameleon slot emulation uses clock/64`);
        }
        type = 100;
      } else {
        label = typeName(type);
      }

      if (bytes.length !== size + 2) {
        throw new Error('Invalid EM410x scan length');
      }
      bytes = bytes.slice(2);
    } else if (protocol.command === 3002) {
      if (bytes.length !== 16 || bytes.slice(13).some(Boolean)) {
        throw new Error('Invalid HID Prox scan length');
      }
      bytes = bytes.slice(0, 13);
    }

    if (bytes.length !== size) {
      throw new Error('Invalid RFID scan length');
    }

    const memory = [Array.from(bytes)];
    return {
      name: `${label}-${hex(bytes)}`,
      warnings,
      lf: { clock },
      type,
      kind: 'lf',
      unitBytes: size,
      card: { uid: [], atqa: [], sak: 0, ats: [] },
      memory
    };
  }

  try {
    const chip = await inspectLfChip(device);
    if (chip) {
      return {
        name: 'T55x7',
        kind: 'lf',
        unitBytes: 1,
        memory: [],
        card: { uid: [], atqa: [], sak: 0, ats: [] },
        lf: { chip },
        warnings: ['Chip detected; no supported broadcast ID decoded']
      };
    }
  } catch (error) {
    if (![0x41, 0x60, 0x67, 0x69].includes(error.status) && error.httpStatus !== 404) {
      throw error;
    }
  }

  throw Object.assign(new Error('No supported RFID tag found. Keep the tag on the antenna and try again.'), {
    status: 0x41
  });
}
