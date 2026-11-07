import { readFile } from 'node:fs/promises';
import { createEngine } from '../../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';
import { encodeLfFields, LF_TYPES } from '../../../../.cache/web/assets/modules/documents/lf.mjs';
import { TYPE2_PROFILES, encodeSlotImage, decodeSlotImage } from '../../src/documents/slot-image.mjs';
import { hfDocument, blankNfcv, applyHfDefaults } from '../../src/documents/hf-image.mjs';
import { desfireDocument, littleEndianBytes } from '../../src/documents/desfire.mjs';

const pattern = (length, seed) => Array.from({ length }, (_, index) => (index * 37 + seed) & 255);

export async function slotMatrixFixtures() {
  const wasm = await readFile(
    new URL('../../../../.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm', import.meta.url)
  );
  const call = await createEngine(wasm);
  const documents = [];
  for (const [sak, uidLength, atsLength] of [
    [0x18, 4, 255],
    [9, 4, 0],
    [0x10, 10, 0],
    [8, 7, 0]
  ]) {
    const geometry = call({ op: 'classic_geometry', sak });
    const uid = pattern(uidLength, 0x12);
    const memory = Array.from({ length: geometry.blocks }, (_, block) => pattern(16, block + 0x20));
    memory[0].splice(
      0,
      8,
      ...uid.slice(0, 4),
      uid.slice(0, 4).reduce((bcc, value) => bcc ^ value),
      sak,
      geometry.atqa[1],
      geometry.atqa[0]
    );
    for (const sector of geometry.sectors) {
      memory[sector.trailer] = [
        ...pattern(6, sector.first),
        0xff,
        7,
        0x80,
        0x69,
        ...pattern(6, sector.first + 1)
      ];
    }
    documents.push({
      type: { 20: 1000, 64: 1001, 128: 1002, 256: 1003 }[geometry.blocks],
      kind: 'classic',
      unitBytes: 16,
      card: { uid, sak, atqa: geometry.atqa, ats: pattern(atsLength, 0x30) },
      memory,
      emulator: { options: [4, 1, 1, 1, 1, 1, 2], extra: [] }
    });
  }
  for (const [type, profile] of Object.entries(TYPE2_PROFILES)) {
    const geometry = call({ op: 'type2_geometry', profile });
    const counters = geometry.version.length ? Math.max(1, geometry.counters.length) : 0;
    const extra = Array.from({ length: counters + (geometry.version.length ? 10 : 0) }, (_, index) =>
      pattern(4, index + 0x50)
    );
    if (geometry.version.length) {
      extra[counters] = geometry.version.slice(0, 4);
      extra[counters + 1] = geometry.version.slice(4);
    }
    const uid = [4, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6];
    const memory = Array.from({ length: geometry.pages }, (_, page) => pattern(4, page + 1));
    memory[0] = [...uid.slice(0, 3), 0x88 ^ uid[0] ^ uid[1] ^ uid[2]];
    memory[1] = uid.slice(3);
    documents.push({
      type: Number(type),
      kind: 'type2',
      unitBytes: 4,
      card: { uid, sak: 0, atqa: [0, 0x44], ats: [] },
      memory,
      emulator: { options: [1, 4, 1, geometry.pages >> 8, geometry.pages & 255], extra }
    });
  }
  for (const type of [1500, 1501]) {
    const document = blankNfcv(type, [0xe0, 4, type === 1500 ? 3 : 1, 0x12, 0x34, 0x56, 0x78, 0x90]);
    document.memory = document.memory.map((_, block) => pattern(4, block + 0x60));
    document.locks = document.memory.map((_, block) => block % 3 === 0);
    applyHfDefaults(document);
    document.emulator.secrets = pattern(document.emulator.secrets.length, 0x70);
    documents.push(document);
  }
  const st25tb = hfDocument({
    protocol: 'st25tb',
    uid: [0xd0, 2, 0x33, 1, 2, 3, 4, 5],
    blocks: 16,
    block_size: 4,
    chip_id: 0x33,
    system: [0xff, 0xff, 0xff, 0xff]
  });
  st25tb.memory = Array.from({ length: 16 }, (_, block) => pattern(4, block + 0x80));
  documents.push(st25tb);

  const capture = JSON.parse(
    await readFile(new URL('../../../../resource/tools/tests/desfire-public.json', import.meta.url), 'utf8')
  );
  // Expand its first public standard file to exactly 4492 native bytes (4497 on wire).
  const sizeOffset = capture.memory.indexOf(64, 19);
  if (sizeOffset < 0 || capture.memory[sizeOffset + 4] !== 64) {
    throw new Error('DESFire fixture layout changed');
  }
  const dataBytes = 4492 - (capture.uid.length + 5 + capture.metadata.length + capture.memory.length - 64);
  capture.memory.splice(sizeOffset, 3, ...littleEndianBytes(dataBytes, 3));
  capture.memory.splice(sizeOffset + 4, 4, ...littleEndianBytes(dataBytes, 4));
  capture.memory.splice(sizeOffset + 8, 64, ...pattern(dataBytes, 0x90));
  documents.push(desfireDocument(capture, 'Synthetic maximum-sized public capture'));

  const lfFields = {
    100: { id: '0102030405' },
    104: { id: '0102030405', epilogue: '1122334455667788' },
    150: { id: 'TEST1234' },
    170: { id: '01020304' },
    180: { id: '0102030405' },
    200: { format: '1', facility: '42', number: '12345', issue: '0', oem: '900' },
    201: { version: '1', facility: '42', number: '12345' },
    310: { id: '01020304' }
  };
  for (const type of LF_TYPES) {
    const memory = encodeLfFields(type, lfFields[type]);
    documents.push({ type, kind: 'lf', unitBytes: memory.length, card: {}, memory: [memory] });
  }
  return documents.map((document) => {
    const bytes = encodeSlotImage(document);
    // Require the app codec to round-trip every synthetic fixture before using hardware.
    if (JSON.stringify([...encodeSlotImage(decodeSlotImage(bytes))]) !== JSON.stringify([...bytes])) {
      throw new Error(`Fixture ${document.type} did not round-trip`);
    }
    return { type: document.type, sense: document.kind === 'lf' ? 1 : 2, bytes: [...bytes] };
  });
}
