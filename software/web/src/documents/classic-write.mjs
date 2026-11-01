import { bytesValid } from './slot-image.mjs';

export function classicReadSnapshot({ card, geometry, memory, summary }) {
  return structuredClone({ card, geometry, memory, keys: summary?.mapping || [] });
}

export function classicWriteGroups(document, writes) {
  const data = [];
  const trailers = [];
  for (const unit of writes) {
    const sector = document.geometry.sectors.findIndex((group) => group.trailer === unit.index);
    if (sector < 0) {
      data.push(unit);
    } else {
      trailers.push({ ...unit, sector });
    }
  }
  return { data, trailers };
}

export function classicChanges(document) {
  const reading = document?.classicRead;
  if (
    document?.kind !== 'classic' ||
    !reading ||
    ![4, 7].includes(reading.card.uid.length) ||
    document.memory.length !== reading.geometry.blocks
  ) {
    return null;
  }

  const writes = [];
  const unknown = [];
  for (const [index, after] of document.memory.entries()) {
    if (index === 0) {
      continue;
    }

    const before = reading.memory[index];
    if (JSON.stringify(before) === JSON.stringify(after)) {
      continue;
    }

    if (
      after?.length !== 16 ||
      !bytesValid(after) ||
      before?.length !== 16 ||
      (reading.geometry.sectors.some((sector) => sector.trailer === index)
        ? !bytesValid(before.slice(6, 10))
        : !bytesValid(before))
    ) {
      unknown.push(index);
    } else {
      writes.push({ index, before: [...before], after: [...after] });
    }
  }

  return { writes, unknown };
}
