import { COMMAND } from './commands.mjs';
import { decodeClassicMagic } from '../documents/classic-magic.mjs';
import { encodeClassicBatch, decodeClassicBatch } from './classic-batch.mjs';
import { scanNfc, scanNfcA } from './hf-reader.mjs';
import { ST25TB, encodeSt25tbTearoff, decodeSt25tbTearoff } from './st25tb-protocol.mjs';

import { DeviceSession, hex, post, nativeClassicJobRunner } from './transport.mjs';
import { createNonceStream } from './nonce-stream.mjs';

export { hex } from './transport.mjs';

export class Device extends DeviceSession {
  constructor(signal) {
    super(signal);
    this.nfcvPassword = null;
    this.keySessionAt = 0;
  }

  async begin(onFamily) {
    try {
      await this.reserve();
      this.card = await (onFamily ? scanNfc(this, onFamily) : scanNfcA(this));
      return this.card;
    } catch (error) {
      await this.end().catch(() => {});
      throw error;
    }
  }

  nfcvPayload(command, payload) {
    return this.nfcvPassword && [COMMAND.NFCV_SYSTEM_INFO, COMMAND.NFCV_DUMP].includes(command)
      ? [...payload, ...this.nfcvPassword]
      : payload;
  }

  async command(command, payload = []) {
    if (this.signal?.aborted) {
      throw new Error('Operation cancelled');
    }
    if (this.error) {
      throw this.error;
    }
    if (!this.session) {
      throw new Error('No device session');
    }

    if (command !== COMMAND.CLASSIC_CHECK_KEYS) {
      this.keySessionAt = 0;
    }

    const bytes = await post(
      `/api/command/${command}?id=${this.session}`,
      new Uint8Array(this.nfcvPayload(command, payload))
    );

    if (command === COMMAND.CLASSIC_CHECK_KEYS && this.keySessionAt) {
      this.keySessionAt = performance.now();
    }
    if (
      this.signal?.aborted &&
      ![
        COMMAND.MAGIC_PROBE,
        COMMAND.CLASSIC_READ,
        COMMAND.CLASSIC_READ_SECTOR,
        COMMAND.CLASSIC_BATCH,
        COMMAND.TYPE2_READ,
        COMMAND.DESFIRE,
        COMMAND.LF_T55XX
      ].includes(command)
    ) {
      throw new Error('Operation cancelled');
    }
    if (bytes.length < 2) {
      throw new Error('Truncated device response');
    }

    const status = (bytes[0] << 8) | bytes[1];
    if (status !== 0 && status !== 0x68 && !(command >= 3000 && status === 0x40)) {
      const error = new Error(
        {
          1: 'Card not found',
          5: 'Invalid UID checksum (BCC). A magic Classic tag may need repair.',
          6: 'Authentication rejected',
          0x41: 'T55xx detection inconclusive. Check placement; protected targets are unsupported.',
          0x42: 'Password-mode targets cannot be programmed by this workflow.',
          0x60: 'Invalid command arguments',
          0x67: 'Firmware does not support this command.',
          0x69: 'This operation is not supported for this configuration.',
          0x76: 'The target changed. Check it again before writing.'
        }[status] || `Device status ${status}`
      );
      error.status = status;
      error.data = bytes.slice(2);
      throw error;
    }

    return bytes.slice(2);
  }

  async probeMagic(card, key = null, keyB = false) {
    if (this.keySessionAt) {
      await this.command(COMMAND.CAPTURE_SESSION, [0]);
    }
    let credential = 0;
    if (key) {
      credential = keyB ? 2 : 1;
    }
    const request = [
      card.uid.length,
      ...card.uid,
      ...Array(10 - card.uid.length).fill(0),
      ...[...card.atqa].reverse(),
      card.sak,
      credential
    ];
    if (key) {
      request.push(...key);
    }
    return decodeClassicMagic(await this.command(COMMAND.MAGIC_PROBE, request));
  }

  async maintenanceUnit(options, block, credential, change = null, check = false) {
    let operation = options.unbound ? 3 : 0;
    let kind = 0;
    if (change) {
      operation = check ? 1 : 2;
    }
    if (credential) {
      kind = credential.keyB ? 2 : 1;
    }
    const request = [
      operation,
      options.method,
      options.model,
      options.uid.length,
      ...options.uid,
      ...Array(7 - options.uid.length).fill(0),
      block,
      kind,
      ...(credential?.key || Array(6).fill(0))
    ];
    if (change) {
      request.push(...change.before, ...change.after);
    }
    if (options.capacityCheck) {
      request.push(1);
    }
    const bytes =
      change && !check
        ? await this.writeCommand(COMMAND.CLASSIC_MAINTENANCE, request)
        : await this.command(COMMAND.CLASSIC_MAINTENANCE, request);
    if (change) {
      if (bytes.length) {
        throw new Error('Invalid maintenance write reply');
      }
      return null;
    }
    if (bytes.length !== 27 || bytes[0] !== options.uid.length) {
      throw new Error('Invalid maintenance read reply');
    }
    const uid = Array.from(bytes.slice(1, 1 + bytes[0]));
    if (!options.unbound && hex(uid) !== hex(options.uid)) {
      throw new Error('The maintenance target changed. No further writes will be sent.');
    }
    options.unbound = false;
    options.uid = uid;
    return {
      card: { uid: [...options.uid], atqa: Array.from(bytes.slice(8, 10)).reverse(), sak: bytes[10] },
      block: Array.from(bytes.slice(11))
    };
  }

  async dump(command, payload) {
    if (
      ![ST25TB.DUMP, COMMAND.NFCV_DUMP].includes(command) ||
      this.signal?.aborted ||
      !this.session ||
      this.error
    ) {
      throw new Error('Device is not ready for a dump');
    }

    const bytes = await post(
      `/api/command/${command}?id=${this.session}`,
      new Uint8Array(this.nfcvPayload(command, payload))
    );

    if (bytes.length < 2) {
      throw new Error('Truncated dump response');
    }

    // The caller retains a validated capture before reporting a pending Stop.
    return { status: (bytes[0] << 8) | bytes[1], data: bytes.slice(2) };
  }

  async writeCommand(command, payload) {
    if (this.signal?.aborted || !this.session || this.error) {
      throw new Error('Device is not ready');
    }

    try {
      const bytes = await post(`/api/command/${command}?id=${this.session}`, new Uint8Array(payload));

      if (bytes.length < 2 || bytes[0] !== 0 || bytes[1] !== 0) {
        const status = bytes.length < 2 ? 'unknown' : (bytes[0] << 8) | bytes[1];
        throw new Error(`Device status ${status}`);
      }

      return bytes.slice(2);
    } catch (error) {
      throw new Error(`Write outcome unknown. Inspect the tag before retrying. ${error.message}`);
    }
  }

  async tearoffStart(uid, block, target) {
    if (!this.session || this.error) {
      throw new Error('No device session');
    }

    await post(`${ST25TB.TEAROFF_PATH}/start?id=${this.session}`, encodeSt25tbTearoff(uid, block, target));
  }

  async tearoffState() {
    return decodeSt25tbTearoff(await post(`${ST25TB.TEAROFF_PATH}/state?id=${this.session}`));
  }

  async tearoffCancel() {
    await post(`${ST25TB.TEAROFF_PATH}/cancel?id=${this.session}`);
  }

  async findKey(sector, isKeyB, candidates, geometry, onTested = () => {}) {
    if (!candidates.length) {
      return null;
    }

    if (!this.keySessionAt || performance.now() - this.keySessionAt >= 1000) {
      await this.command(COMMAND.CAPTURE_SESSION, [1]);
      this.keySessionAt = performance.now();
    }

    for (let offset = 0; offset < candidates.length; offset += 42) {
      if (this.signal?.aborted) {
        throw new Error('Operation cancelled');
      }

      const batch = candidates.slice(offset, offset + 42);

      try {
        const response = await this.command(COMMAND.CLASSIC_CHECK_KEYS, [
          geometry.sectors[sector].first,
          isKeyB ? 0x61 : 0x60,
          batch.length,
          ...batch.flat()
        ]);

        if (response.length !== 7 || response[0] !== 1) {
          throw new Error('Invalid key verification reply');
        }

        const key = Array.from(response.slice(1));
        const matchedIndex = batch.findIndex((candidate) => hex(candidate) === hex(key));

        if (matchedIndex === -1) {
          throw new Error('Device returned an unsubmitted key');
        }

        onTested(batch.slice(0, matchedIndex + 1));
        return key;
      } catch (error) {
        if (error.status !== 6) {
          throw error;
        }

        onTested(batch);
      }
    }

    return null;
  }

  async classicBatch(kind, entries) {
    const payload = encodeClassicBatch(kind, this.card.uid, entries);
    const bytes = await this.command(COMMAND.CLASSIC_BATCH, payload);
    return decodeClassicBatch(bytes, kind, entries);
  }

  async readBlock(block, key, isKeyB) {
    const bytes = await this.command(COMMAND.CLASSIC_READ, [isKeyB ? 0x61 : 0x60, block, ...key]);

    if (bytes.length !== 16) {
      throw new Error('Invalid block response');
    }

    return Array.from(bytes);
  }

  async readSector(sector, key, isKeyB, geometry) {
    const { count } = geometry.sectors[sector];
    const bytes = await this.command(COMMAND.CLASSIC_READ_SECTOR, [
      isKeyB ? 0x61 : 0x60,
      sector,
      ...key,
      this.card.uid.length,
      ...this.card.uid
    ]);

    if (bytes.length !== 2 + count * 16) {
      throw new Error('Invalid sector response');
    }

    const mask = (bytes[0] << 8) | bytes[1];

    if (mask >>> count) {
      throw new Error('Invalid sector block mask');
    }

    return Array.from({ length: count }, (_, index) => {
      if (!(mask & (1 << index))) {
        return null;
      }

      const offset = 2 + index * 16;
      return Array.from(bytes.slice(offset, offset + 16));
    });
  }

  async collectNonces(source, target, count, geometry, chained = false) {
    const bytes = await this.command(COMMAND.CLASSIC_COLLECT, [
      source.is_key_b ? 0x61 : 0x60,
      geometry.sectors[source.sector].first,
      ...source.key,
      target.is_key_b ? 0x61 : 0x60,
      geometry.sectors[target.sector].first,
      Number(chained),
      count
    ]);

    if (bytes.length !== count * 12) {
      throw new Error('Invalid capture length');
    }

    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return Array.from({ length: count }, (_, index) => {
      const offset = index * 12;
      const parity = Array.from(bytes.slice(offset + 8, offset + 12));

      if (parity.some((bit) => bit > 1)) {
        throw new Error('Invalid capture parity');
      }

      return { nt1: view.getUint32(offset), ciphertext: view.getUint32(offset + 4), parity };
    });
  }

  startNonceStream(source, target, count, geometry, onProgress) {
    const run = nativeClassicJobRunner();
    if (!run) {
      return null;
    }
    this.check();
    if (!this.session || !Number.isInteger(count) || count < 1 || count > 4096) {
      throw new Error('Device is not ready for nonce acquisition');
    }
    this.keySessionAt = 0;
    const body = [
      1,
      2,
      this.card.uid.length,
      ...this.card.uid,
      source.is_key_b ? 0x61 : 0x60,
      geometry.sectors[source.sector].first,
      ...source.key,
      target.is_key_b ? 0x61 : 0x60,
      geometry.sectors[target.sector].first,
      0,
      16,
      count >> 8,
      count & 255
    ];
    return createNonceStream({
      run,
      lease: this.session,
      body,
      budget: count,
      signal: this.signal,
      onProgress
    });
  }

  async captureDarkside(target, variant, geometry) {
    const payload = [
      target.is_key_b ? 0x61 : 0x60,
      geometry.sectors[target.sector].first,
      variant === 0 ? 0 : 2,
      this.card.uid.length,
      ...this.card.uid
    ];

    for (let step = 0; step < 512; step++) {
      const bytes = await this.command(COMMAND.DARKSIDE_STEP, payload);

      if (bytes.length === 1 && bytes[0] === 5) {
        payload[2] = 1;
        continue;
      }
      if (bytes.length === 1 && [1, 2, 3, 6].includes(bytes[0])) {
        return null;
      }
      if (bytes.length === 1 && bytes[0] === 4) {
        throw new Error('Card changed during Darkside');
      }
      if (bytes.length !== 33 || bytes[0] !== 0 || bytes.slice(17, 25).some((byte) => byte > 15)) {
        throw new Error('Invalid Darkside capture');
      }
      if (hex(bytes.slice(1, 5)) !== hex(this.card.uid.slice(-4))) {
        throw new Error('Darkside observation UID mismatch');
      }

      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      return {
        uid: view.getUint32(1),
        nt: view.getUint32(5),
        par_list: Array.from(bytes.slice(9, 17)),
        ks_list: Array.from(bytes.slice(17, 25)),
        nr: view.getUint32(25),
        ar: view.getUint32(29)
      };
    }

    throw new Error('Darkside acquisition exceeded its attempt budget');
  }

  async end() {
    this.nfcvPassword = null;
    this.keySessionAt = 0;
    await super.end();
  }
}
