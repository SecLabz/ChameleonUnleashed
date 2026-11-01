import { withRuntime } from '../runtime/runtime.mjs';
import { COMMAND } from './commands.mjs';
import { type2Document, TYPE2_NOTES } from '../documents/workspace.mjs';

import { TYPE2_PROFILES } from '../documents/slot-image.mjs';

// The caller owns Device.begin/end. End releases RF even after cancellation.
export async function readType2(device, card, profile, onUpdate) {
  const request = (operation, parameter = 0, endPage) =>
    device.command(COMMAND.TYPE2_READ, [
      operation,
      parameter,
      card.uid.length,
      ...card.uid,
      ...(endPage === undefined ? [] : [endPage])
    ]);

  let version;
  if (profile === 'auto') {
    version = Array.from(await request(0));
  }

  const geometry = await withRuntime(
    (call) => call({ op: 'type2_geometry', ...(profile === 'auto' ? { version } : { profile }) }),
    device.signal
  );
  const type = Number(Object.keys(TYPE2_PROFILES).find((type) => TYPE2_PROFILES[type] === geometry.profile));
  if (!type) {
    throw new Error('Unsupported Type 2 emulator profile');
  }

  const hasVersion = geometry.version.length > 0;
  const counterCount = hasVersion ? Math.max(1, geometry.counters.length) : 0;
  const document = type2Document(type, card, geometry);
  const extra = document.emulator.extra;
  if (profile !== 'auto') {
    document.warnings.push('Version/geometry use the manually selected emulator profile');
  }
  onUpdate(document, 'Reading Type 2 memory…');

  const optionalRead = async (operation, parameter, endPage) => {
    try {
      return Array.from(await request(operation, parameter, endPage));
    } catch (error) {
      if ([2, 3, 7].includes(error.status)) {
        return null;
      }
      throw error;
    }
  };

  let finished = false;
  try {
    await device.command(COMMAND.CAPTURE_SESSION, [1]);

    const supportsFastRead = ['ntag213', 'ntag215', 'ntag216'].includes(geometry.profile);
    for (let page = 0; page < geometry.pages; ) {
      if (geometry.credentials.includes(page)) {
        page++;
        continue;
      }

      let pageCount = 1;
      if (supportsFastRead) {
        while (
          pageCount < 8 &&
          page + pageCount < geometry.pages &&
          !geometry.credentials.includes(page + pageCount)
        ) {
          pageCount++;
        }
      }

      const bytes = await optionalRead(
        supportsFastRead ? 5 : 1,
        page,
        supportsFastRead ? page + pageCount - 1 : undefined
      );
      const expectedBytes = supportsFastRead ? pageCount * 4 : 16;
      if (bytes && bytes.length !== expectedBytes) {
        throw new Error('Invalid Type 2 page response');
      }

      if (bytes) {
        // READ can wrap at access boundaries; FAST_READ uses an explicit range.
        for (let offset = 0; offset < pageCount; offset++) {
          document.memory[page + offset] = bytes.slice(offset * 4, offset * 4 + 4);
        }
      } else if (supportsFastRead && pageCount > 1) {
        for (let offset = 0; offset < pageCount; offset++) {
          const single = await optionalRead(5, page + offset, page + offset);
          if (single && single.length !== 4) {
            throw new Error('Invalid Type 2 page response');
          }
          document.memory[page + offset] = single;
          onUpdate(document, `Reading Type 2 page ${page + offset + 1}/${geometry.pages}`);
        }
      }

      page += pageCount;
      if (supportsFastRead || page % 4 === 0 || page === geometry.pages) {
        onUpdate(document, `Reading Type 2 page ${page}/${geometry.pages}`);
      }
    }

    for (const [extraIndex, counterIndex] of geometry.counters.entries()) {
      const bytes = await optionalRead(3, counterIndex);
      if (bytes && bytes.length !== 3) {
        throw new Error('Invalid Type 2 counter response');
      }

      let tearing = [0xbd];
      if (geometry.tearing) {
        tearing = await optionalRead(4, counterIndex);
      }
      if (tearing && (tearing.length !== 1 || ![0, 0xbd].includes(tearing[0]))) {
        throw new Error('Invalid Type 2 tearing response');
      }
      if (bytes && tearing) {
        extra[extraIndex] = [...bytes, tearing[0] === 0xbd ? 0 : 0x80];
      }
    }

    // NTAG210/212 reserve an emulator auth-attempt cell but have no RF counter.
    if (counterCount && !geometry.counters.length) {
      extra[0] = [0, 0, 0, 0];
    }

    if (hasVersion) {
      const signature = await optionalRead(2, 0);
      if (signature && signature.length !== 32) {
        throw new Error('Invalid Type 2 signature response');
      }
      if (signature) {
        for (let page = 0; page < 8; page++) {
          extra[counterCount + 2 + page] = signature.slice(page * 4, page * 4 + 4);
        }
      }
    }

    const unread = document.memory.filter(
      (bytes, page) => !bytes && !geometry.credentials.includes(page)
    ).length;
    if (geometry.credentials.length) {
      document.warnings.push(TYPE2_NOTES.credentials);
    }
    if (unread) {
      document.warnings.push(`${unread} user/configuration pages unread`);
    }
    if (extra.some((page) => !page)) {
      document.warnings.push(TYPE2_NOTES.metadata);
    }

    finished = true;
    onUpdate(document, 'Type 2 read finished');
    return document;
  } finally {
    document.type2Read = { type, card: structuredClone(card), memory: structuredClone(document.memory) };
    if (!finished) {
      onUpdate(document, 'Type 2 read interrupted · completed pages retained');
    }
  }
}
