// Synthetic volatile PM3 images. Each new size requires its own stop preflight.
export const uid = [0x11, 0x22, 0x33, 0x44];
export const keyA = Array(6).fill(0xff);
export const keyB = [0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5];
export const access = [0x7f, 0x07, 0x88, 0x69]; // Data 000; trailer 011, with hidden Key B.

export const classicModels = {
  mini: { type: 1000, sak: 9, atqa: [4, 0], sectors: 5, blocks: 20 },
  '1k': { type: 1001, sak: 8, atqa: [4, 0], sectors: 16, blocks: 64 },
  '2k': {
    type: 1002,
    sak: 8,
    atqa: [4, 0],
    sectors: 32,
    blocks: 128,
    ats: [12, 0x75, 0x77, 0x80, 2, 0xc1, 5, 0x2f, 0x2f, 1, 0xbc, 0xd6]
  },
  '4k': { type: 1003, sak: 0x18, atqa: [2, 0], sectors: 40, blocks: 256 }
};

export function classicFixture(size = '1k') {
  const model = Object.hasOwn(classicModels, size) && classicModels[size];
  if (!model) {
    throw new Error('Unsupported PM3 fixture geometry');
  }
  const sectors = Array.from({ length: model.sectors }, (_, index) => {
    const first = index < 32 ? index * 4 : 128 + (index - 32) * 16;
    const count = index < 32 ? 4 : 16;
    return { first, count, trailer: first + count - 1 };
  });
  const blocks = Array.from({ length: model.blocks }, (_, block) =>
    Array.from({ length: 16 }, (_, offset) => (block * 17 + offset * 7 + 3) & 255)
  );
  blocks[0] = [...uid, 0x44, model.sak, ...model.atqa, ...blocks[0].slice(8)];
  for (const sector of sectors) {
    blocks[sector.trailer] = [...keyA, ...access, ...keyB];
  }
  const bytes = Buffer.alloc(4096);
  bytes.set(blocks.flat());
  return { blocks, bytes, sectors, model };
}
