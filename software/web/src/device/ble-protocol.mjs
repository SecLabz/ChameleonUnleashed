// Versioned web-service envelope carried by the Ultra's Nordic UART service.
// Keep operation IDs aligned with firmware/application/src/net/ble_api.h.
export const BLE_API = {
  VERSION: 1,
  COMMAND: 1090,
  JOB_EVENT: 1091,
  MAX_BODY: 1024,
  MAX_RESPONSE: 512
};

export const OPERATION = {
  HELLO: 0,
  DEVICE: 1,
  SLOT: 2,
  STATUS: 3,
  READER_SESSION: 4,
  SLOT_SESSION: 5,
  KEEPALIVE: 6,
  END: 7,
  COMMAND: 8,
  SETTINGS: 9,
  SLOT_OPEN: 10,
  SLOT_READ: 11,
  SLOT_BEGIN: 12,
  SLOT_WRITE: 13,
  SLOT_COMMIT: 14,
  SLOT_ACTIVATE: 15,
  SLOT_NICK: 16,
  SLOT_ENABLE: 17,
  SLOT_CLEAR: 18,
  TEAROFF_START: 19,
  TEAROFF_STATE: 20,
  TEAROFF_CANCEL: 21,
  UPDATE_INFO: 22,
  TRANSPORT_INFO: 23,
  LINK_PROFILE: 24,
  CLASSIC_JOB_START: 25,
  CLASSIC_JOB_CANCEL: 26,
  CLASSIC_JOB_READ: 27
};

const checksum = (bytes) => -bytes.reduce((sum, byte) => sum + byte, 0) & 255;

export function encodeFrame(command, payload = new Uint8Array(), status = 0) {
  if (payload.length > 4096) {
    throw new Error('Bluetooth frame is too large');
  }
  const bytes = new Uint8Array(payload.length + 10);
  const view = new DataView(bytes.buffer);
  bytes.set([0x11, 0xef]);
  view.setUint16(2, command);
  view.setUint16(4, status);
  view.setUint16(6, payload.length);
  bytes[8] = checksum(bytes.slice(0, 8));
  bytes.set(payload, 9);
  bytes[bytes.length - 1] = checksum(payload);
  return bytes;
}

export class FrameDecoder {
  constructor() {
    this.buffer = new Uint8Array();
  }

  push(value) {
    const incoming = ArrayBuffer.isView(value)
      ? new Uint8Array(value.buffer, value.byteOffset, value.byteLength)
      : new Uint8Array(value);
    if (this.buffer.length + incoming.length > 8212) {
      throw new Error('Bluetooth receive buffer overflow');
    }
    const joined = new Uint8Array(this.buffer.length + incoming.length);
    joined.set(this.buffer);
    joined.set(incoming, this.buffer.length);
    this.buffer = joined;
    const frames = [];
    while (this.buffer.length >= 9) {
      const bytes = this.buffer;
      if (bytes[0] !== 0x11 || bytes[1] !== 0xef || checksum(bytes.slice(0, 9))) {
        throw new Error('Invalid Bluetooth frame header');
      }
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const size = view.getUint16(6);
      if (size > 4096) {
        throw new Error('Bluetooth response is too large');
      }
      if (bytes.length < size + 10) {
        break;
      }
      if (checksum(bytes.slice(9, size + 10))) {
        throw new Error('Invalid Bluetooth response checksum');
      }
      frames.push({ command: view.getUint16(2), status: view.getUint16(4), data: bytes.slice(9, size + 9) });
      this.buffer = bytes.slice(size + 10);
    }
    return frames;
  }
}

export function encodeOperation(operation, correlation, lease = 0, argument = 0, body = new Uint8Array()) {
  if (
    !Number.isInteger(correlation) ||
    correlation < 1 ||
    correlation > 0xffffffff ||
    !Number.isInteger(lease) ||
    lease < 0 ||
    lease > 0xffffffff ||
    !Number.isInteger(argument) ||
    argument < 0 ||
    argument > 0xffff ||
    body.length > BLE_API.MAX_BODY
  ) {
    throw new Error('Invalid Bluetooth service request');
  }
  const bytes = new Uint8Array(12 + body.length);
  const view = new DataView(bytes.buffer);
  bytes[0] = BLE_API.VERSION;
  view.setUint32(1, correlation);
  bytes[5] = operation;
  view.setUint32(6, lease);
  view.setUint16(10, argument);
  bytes.set(body, 12);
  return bytes;
}

const getRoutes = {
  '/api/device': OPERATION.DEVICE,
  '/api/status': OPERATION.STATUS,
  '/api/update': OPERATION.UPDATE_INFO
};
const postRoutes = {
  '/api/session': OPERATION.READER_SESSION,
  '/api/slots/session': OPERATION.SLOT_SESSION,
  '/api/settings': OPERATION.SETTINGS
};

export function encodeRequest(path, data, correlation) {
  let operation;
  let lease = 0;
  let argument = 0;
  if (data === undefined) {
    operation = getRoutes[path];
    const slot = /^\/api\/slot\/([0-7])$/.exec(path);
    if (slot) {
      operation = OPERATION.SLOT;
      argument = Number(slot[1]);
    }
  } else {
    operation = postRoutes[path];
    const job = /^\/api\/classic\/job\/(start|cancel|read)\?id=([1-9]\d*)$/.exec(path);
    if (job) {
      operation = OPERATION[`CLASSIC_JOB_${job[1].toUpperCase()}`];
      lease = Number(job[2]);
    }
    const query =
      /^\/api\/(keepalive|end|command\/(\d+)|slots\/(open|read|begin|write|commit|activate|nick|enable|clear)|st25tb\/tearoff\/(start|state|cancel))\?id=([1-9]\d*)$/.exec(
        path
      );
    if (query) {
      lease = Number(query[5]);
      if (query[2] !== undefined) {
        operation = OPERATION.COMMAND;
        argument = Number(query[2]);
      } else if (query[3]) {
        operation = OPERATION[`SLOT_${query[3].toUpperCase()}`];
      } else if (query[4]) {
        operation = OPERATION[`TEAROFF_${query[4].toUpperCase()}`];
      } else {
        operation = OPERATION[query[1].toUpperCase()];
      }
    }
  }
  if (operation === undefined) {
    throw new Error(
      path.startsWith('/api/update')
        ? 'Use Firmware update to connect to Nordic DFU, or use USB recovery.'
        : `Unsupported Bluetooth operation: ${path}`
    );
  }
  return encodeOperation(operation, correlation, lease, argument, data);
}

export function decodeReply(frame, correlation) {
  if (frame.command !== BLE_API.COMMAND || frame.status !== 0x68) {
    throw new Error(
      'This firmware does not support the Web Bluetooth service. Update the firmware through Firmware update or USB DFU.'
    );
  }
  const bytes = frame.data;
  if (bytes.length < 8 || bytes.length > BLE_API.MAX_RESPONSE + 8 || bytes[0] !== BLE_API.VERSION) {
    throw new Error('Invalid Bluetooth service response');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(1) !== correlation || bytes[7] > 1) {
    throw new Error('Bluetooth response belongs to a different request');
  }
  return { status: view.getUint16(5), json: !!bytes[7], data: bytes.slice(8) };
}
