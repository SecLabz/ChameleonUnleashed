// Optional command-2027 diagnostic reply. Production callers keep their
// unchanged mask/data contract; this decoder is for explicit bench measurements.
export function decodeSectorDiagnostic(reply, count) {
  const bytes = Array.from(reply);
  if (
    ![4, 16].includes(count) ||
    bytes.length < 2 ||
    !bytes.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 255)
  ) {
    throw new Error('Invalid sector diagnostic bytes');
  }
  const rfStatus = bytes[0] * 256 + bytes[1];
  if (rfStatus) {
    if (bytes.length !== 2) {
      throw new Error('Invalid failed sector diagnostic response');
    }
    if ([0x60, 0x67, 0x69].includes(rfStatus)) {
      throw new Error('Update the firmware for optional sector diagnostics');
    }
    return { rfStatus, blockMask: null, blockReadStatus: null, firstFailedOffset: null, blocks: null };
  }
  if (bytes.length !== 8 + count * 16 || bytes.at(-4) !== 1) {
    throw new Error('Invalid sector diagnostic format');
  }
  const blockMask = bytes[2] * 256 + bytes[3];
  const failed = bytes.at(-3);
  const blockReadStatus = bytes.at(-2) * 256 + bytes.at(-1);
  const complete = failed === 0xff;
  if (
    (complete && (blockMask !== (1 << count) - 1 || blockReadStatus !== 0)) ||
    (!complete && (failed >= count || blockMask !== (1 << failed) - 1 || blockReadStatus === 0))
  ) {
    throw new Error('Inconsistent sector diagnostic result');
  }
  const blocks = Array.from({ length: count }, (_, offset) => {
    const bytesInBlock = bytes.slice(4 + offset * 16, 20 + offset * 16);
    if (blockMask & (1 << offset)) {
      return bytesInBlock;
    }
    if (bytesInBlock.some((byte) => byte !== 0)) {
      throw new Error('Unknown sector bytes must be zero');
    }
    return null;
  });
  return { rfStatus, blockMask, blockReadStatus, firstFailedOffset: complete ? null : failed, blocks };
}
