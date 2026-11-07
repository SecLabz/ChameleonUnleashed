export function encodeClassicBatch(kind, uid, entries) {
  if (
    ![0, 1].includes(kind) ||
    ![4, 7, 10].includes(uid?.length) ||
    !entries.length ||
    entries.length > 8 ||
    entries.some(
      (entry) =>
        entry.length !== 8 ||
        entry.some((value) => !Number.isInteger(value) || value < 0 || value > 255) ||
        ![0x60, 0x61].includes(entry[1]) ||
        (kind === 1 && entry[0] >= 40)
    )
  ) {
    throw new Error('Invalid Classic batch request');
  }
  return [1, kind, uid.length, ...uid, entries.length, ...entries.flat()];
}

export function decodeClassicBatch(bytes, kind, entries) {
  if (bytes.length < 3 || bytes[0] !== 1 || bytes[1] !== kind || !bytes[2] || bytes[2] > entries.length) {
    throw new Error('Invalid Classic batch reply');
  }
  let offset = 3;
  const results = [];
  for (let i = 0; i < bytes[2]; i++) {
    if (offset + 4 > bytes.length) {
      throw new Error('Truncated Classic batch reply');
    }
    const status = bytes[offset] * 256 + bytes[offset + 1];
    const size = bytes[offset + 2] * 256 + bytes[offset + 3];
    offset += 4;
    if (offset + size > bytes.length) {
      throw new Error('Truncated Classic batch result');
    }
    const data = bytes.slice(offset, offset + size);
    offset += size;
    if (
      kind === 0 &&
      [0, 6].includes(status) &&
      (size !== 7 ||
        data[0] !== Number(status === 0) ||
        (status === 0 && data.slice(1).some((v, n) => v !== entries[i][2 + n])))
    ) {
      throw new Error('Invalid batched authentication result');
    }
    if (kind === 1 && status === 0) {
      const count = entries[i][0] < 32 ? 4 : 16;
      if (size !== 2 + count * 16 || (data[0] * 256 + data[1]) >>> count) {
        throw new Error('Invalid batched sector result');
      }
    }
    results.push({ status, data });
  }
  if (
    offset !== bytes.length ||
    (results.length !== entries.length && [0, 6].includes(results.at(-1).status))
  ) {
    throw new Error('Incomplete Classic batch reply');
  }
  return results;
}
