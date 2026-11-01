import { DeviceSession, request } from './transport.mjs';
import {
  be16,
  be32,
  read16,
  read32,
  decodeSlotImage,
  encodeSlotImage,
  nickname
} from '../documents/slot-image.mjs';

export function decodeSlot(bytes) {
  if (bytes.length < 15 || bytes[4] > 3) {
    throw new Error('Invalid slot response');
  }

  let offset = 5;

  const readName = () => {
    const size = bytes[offset++];
    if (size > 32 || offset + size > bytes.length) {
      throw new Error('Invalid slot nickname');
    }
    const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes.slice(offset, offset + size));
    offset += size;
    return text;
  };

  const hfName = readName();
  const lfName = readName();
  if (offset + 8 !== bytes.length) {
    throw new Error('Invalid slot response length');
  }

  return {
    hf: {
      type: bytes[0] * 256 + bytes[1],
      enabled: !!(bytes[4] & 1),
      name: hfName,
      revision: read32(bytes.slice(offset, offset + 4))
    },
    lf: {
      type: bytes[2] * 256 + bytes[3],
      enabled: !!(bytes[4] & 2),
      name: lfName,
      revision: read32(bytes.slice(offset + 4, offset + 8))
    }
  };
}

export class SlotDevice extends DeviceSession {
  async begin() {
    await this.reserve('/api/slots/session');
  }

  async command(operation, bytes = []) {
    this.check();
    if (!this.session) {
      throw new Error('No slot reservation');
    }

    const result = await request(`/api/slots/${operation}?id=${this.session}`, new Uint8Array(bytes));
    this.check();
    return result;
  }

  async action(operation, bytes) {
    const result = await this.command(operation, bytes);
    if (result.length !== 2 || result[0] !== 0 || result[1] !== 0x68) {
      throw new Error(
        'Slot change unconfirmed. The target may be disabled or partly updated; refresh before retrying.'
      );
    }
  }

  async load(slot, sense, name, revision) {
    const result = await this.command('open', [
      slot,
      sense,
      ...(revision === undefined ? [] : be32(revision))
    ]);
    const size = read16(result);
    if (result.length !== 6 || size < 9 || size > 4500) {
      throw new Error('Invalid slot snapshot size');
    }

    const bytes = new Uint8Array(size);
    for (let offset = 0; offset < size; offset += 512) {
      const count = Math.min(512, size - offset);
      const part = await this.command('read', [...be16(offset), ...be16(count)]);
      if (part.length !== count) {
        throw new Error('Truncated slot snapshot');
      }
      bytes.set(part, offset);
    }

    return { ...decodeSlotImage(bytes, name), revision: read32(result.slice(2)) };
  }

  async save(slot, sense, document, name, revision, enabled = true) {
    const bytes = encodeSlotImage(document);
    const label = nickname(name);

    await this.command('begin', [slot, sense, ...be16(bytes.length), ...be32(revision)]);
    for (let offset = 0; offset < bytes.length; offset += 1000) {
      const part = bytes.slice(offset, offset + 1000);
      const result = await this.command('write', [...be16(offset), ...part]);
      if (result.length !== 2 || read16(result) !== offset + part.length) {
        throw new Error('Slot upload offset mismatch');
      }
    }

    await this.action('commit', [+enabled, label.length, ...label]);
    const saved = await this.load(slot, sense, name);
    const actual = encodeSlotImage(saved);
    if (actual.length !== bytes.length || actual.some((byte, index) => byte !== bytes[index])) {
      throw new Error('Stored image verification failed; refresh before retrying.');
    }

    return saved;
  }
}
