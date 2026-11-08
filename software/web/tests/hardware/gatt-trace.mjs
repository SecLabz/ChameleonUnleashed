import { writeFile } from 'node:fs/promises';
import { FrameDecoder, decodeReply, BLE_API, OPERATION } from '../../src/device/ble-protocol.mjs';

// Observe the real NUS characteristics. The original native methods and all
// transmitted bytes are preserved; this does not enable BluetoothEmulation.
export async function traceGatt(page) {
  await page.addInitScript(() => {
    const chunks = [];
    const watched = new WeakSet();
    let total = 0;
    let truncated = false;
    globalThis.pm3GattTrace = () => ({ chunks, truncated });
    const record = (direction, value) => {
      const bytes = ArrayBuffer.isView(value)
        ? new Uint8Array(value.buffer, value.byteOffset, value.byteLength)
        : new Uint8Array(value);
      total += bytes.length;
      if (total > 1000000) {
        truncated = true;
        return;
      }
      chunks.push({ direction, ms: performance.now(), bytes: [...bytes] });
    };
    const prototype = BluetoothRemoteGATTCharacteristic.prototype;
    for (const method of ['writeValueWithResponse', 'writeValueWithoutResponse']) {
      const write = prototype[method];
      if (typeof write !== 'function') {
        continue;
      }
      prototype[method] = function (value) {
        if (this.uuid === '6e400002-b5a3-f393-e0a9-e50e24dcca9e') {
          record('request', value);
        }
        return write.call(this, value);
      };
    }
    const start = prototype.startNotifications;
    prototype.startNotifications = function () {
      if (this.uuid === '6e400003-b5a3-f393-e0a9-e50e24dcca9e' && !watched.has(this)) {
        watched.add(this);
        this.addEventListener('characteristicvaluechanged', (event) => record('reply', event.target.value));
      }
      return start.call(this);
    };
  });
  return async (filename) => {
    const raw = await page.evaluate(() => globalThis.pm3GattTrace());
    const decoders = { request: new FrameDecoder(), reply: new FrameDecoder() };
    const requests = [];
    const streamEvents = [];
    const pending = new Map();
    const errors = [];
    for (const chunk of raw.chunks) {
      try {
        for (const frame of decoders[chunk.direction].push(Uint8Array.from(chunk.bytes))) {
          if (frame.command === BLE_API.JOB_EVENT) {
            if (frame.status !== 0x68 || frame.data.length < 13) {
              throw new Error('Invalid job notification');
            }
            streamEvents.push({ ms: chunk.ms, bytes: [...frame.data] });
            continue;
          }
          if (frame.command !== BLE_API.COMMAND || frame.data.length < 5) {
            throw new Error('Unexpected NUS frame');
          }
          const view = new DataView(frame.data.buffer, frame.data.byteOffset, frame.data.byteLength);
          const correlation = view.getUint32(1);
          if (chunk.direction === 'request') {
            if (frame.data.length < 12 || pending.has(correlation)) {
              throw new Error('Invalid or duplicate GATT request');
            }
            const record = {
              correlation,
              ms: chunk.ms,
              operation: frame.data[5],
              lease: view.getUint32(6),
              argument: view.getUint16(10),
              body: [...frame.data.slice(12)]
            };
            requests.push(record);
            pending.set(correlation, record);
          } else {
            const request = pending.get(correlation);
            if (!request) {
              throw new Error('Unmatched GATT reply');
            }
            const reply = decodeReply(frame, correlation);
            request.elapsedMs = chunk.ms - request.ms;
            request.httpStatus = reply.status;
            request.reply = [...reply.data];
            if (request.operation === OPERATION.COMMAND && reply.data.length >= 2) {
              request.rfStatus = reply.data[0] * 256 + reply.data[1];
            }
            pending.delete(correlation);
          }
        }
      } catch (error) {
        errors.push({ ms: chunk.ms, direction: chunk.direction, message: error.message });
      }
    }
    const trace = { ...raw, requests, streamEvents, errors, pending: [...pending.keys()] };
    await writeFile(filename, JSON.stringify(trace, null, 2));
    return trace;
  };
}
