/** @param {number} id */
export const jobIdBytes = (id) => Uint8Array.of(id >>> 24, id >>> 16, id >>> 8, id);

/** @param {Uint8Array} bytes @param {number} id @param {number} sequence @param {number} kind
 * @param {number} previous */
export function decodeClassicJob(bytes, id, sequence, kind, previous = 0) {
  if (
    bytes.length < 13 ||
    bytes.length > 285 ||
    bytes[0] !== 1 ||
    bytes[8] !== kind ||
    ![0, 1, 3].includes(bytes[7])
  ) {
    throw new Error('Invalid Classic job event');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(1) !== id || view.getUint16(5) !== sequence) {
    throw new Error('Classic job event belongs to another job or sequence');
  }
  const event = {
    done: !!(bytes[7] & 1),
    cancelled: !!(bytes[7] & 2),
    kind,
    status: view.getUint16(9),
    completed: view.getUint16(11),
    data: bytes.slice(13)
  };
  const advance = event.completed - previous;
  const ordinary = event.status === 0 || (kind !== 2 && event.status === 6);
  if (advance < 0 || (!ordinary && !event.done)) {
    throw new Error('Invalid Classic job progress');
  }
  if (event.cancelled || !ordinary) {
    if (advance || event.data.length) {
      throw new Error('Invalid Classic job termination');
    }
  } else if (kind === 2) {
    if (
      advance < 1 ||
      advance > 16 ||
      event.data.length !== advance * 12 ||
      event.data.some((value, i) => i % 12 >= 8 && value > 1)
    ) {
      throw new Error('Invalid Classic capture event');
    }
  } else if (advance !== 1) {
    throw new Error('Invalid Classic job result count');
  } else if (kind === 0) {
    if (event.data.length !== 7 || event.data[0] !== Number(event.status === 0)) {
      throw new Error('Invalid Classic authentication event');
    }
  } else if (event.status === 0) {
    let count = 0;
    if (event.data.length === 66) {
      count = 4;
    }
    if (event.data.length === 258) {
      count = 16;
    }
    if (!count || (event.data[0] * 256 + event.data[1]) >>> count) {
      throw new Error('Invalid Classic sector event');
    }
  } else if (event.data.length) {
    throw new Error('Invalid failed Classic sector event');
  }
  return event;
}
