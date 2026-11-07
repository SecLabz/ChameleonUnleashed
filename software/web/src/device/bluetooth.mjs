import {
  BLE_API,
  OPERATION,
  FrameDecoder,
  encodeFrame,
  encodeOperation,
  encodeRequest,
  decodeReply
} from './ble-protocol.mjs';
import { HttpError } from './transport.mjs';

export const NORDIC_UART = {
  service: '6e400001-b5a3-f393-e0a9-e50e24dcca9e',
  rx: '6e400002-b5a3-f393-e0a9-e50e24dcca9e',
  tx: '6e400003-b5a3-f393-e0a9-e50e24dcca9e'
};

export class BluetoothTransport {
  constructor(device, rx, tx, { onDisconnect = () => {}, timeoutMs = 20000 } = {}) {
    this.device = device;
    this.rx = rx;
    this.tx = tx;
    this.onDisconnect = onDisconnect;
    this.timeoutMs = timeoutMs;
    this.fragmentBytes = 20;
    this.writeWithoutResponse =
      !!rx.properties?.writeWithoutResponse && typeof rx.writeValueWithoutResponse === 'function';
    this.closed = false;
    this.sequence = 0;
    this.queue = Promise.resolve();
    this.decoder = new FrameDecoder();
    this.pending = null;
    this.disconnected = () => this.disconnect(new Error('Bluetooth disconnected. Reconnect to continue.'));
    this.received = (event) => {
      try {
        for (const frame of this.decoder.push(event.target.value)) {
          if (!this.pending || this.pending.responded) {
            throw new Error('Unexpected Bluetooth response. Reconnect before retrying.');
          }
          const reply = decodeReply(frame, this.pending.correlation);
          this.pending.responded = true;
          this.pending.resolve(reply);
        }
      } catch (error) {
        this.disconnect(error);
      }
    };
  }

  async initialize() {
    this.device.addEventListener('gattserverdisconnected', this.disconnected);
    this.tx.addEventListener('characteristicvaluechanged', this.received);
    try {
      await this.tx.startNotifications();
      const hello = await this.run((correlation) => encodeOperation(OPERATION.HELLO, correlation));
      if (hello.status !== 200 || hello.json || hello.data.length !== 6) {
        throw new Error('Unsupported Web Bluetooth firmware contract. Update the firmware over USB.');
      }
      const limits = new DataView(hello.data.buffer, hello.data.byteOffset, hello.data.byteLength);
      if (
        hello.data[0] !== BLE_API.VERSION ||
        limits.getUint16(1) < BLE_API.MAX_BODY ||
        limits.getUint16(3) < BLE_API.MAX_RESPONSE
      ) {
        throw new Error('Incompatible Web Bluetooth firmware limits. Update the firmware over USB.');
      }
      this.capabilities = { tearoff: !!(hello.data[5] & 1) };
      if (hello.data[5] & 2) {
        const info = await this.run((correlation) => encodeOperation(OPERATION.TRANSPORT_INFO, correlation));
        const limit = info.data.length === 3 ? info.data[1] * 256 + info.data[2] : 0;
        if (info.status !== 200 || info.json || info.data[0] !== 1 || limit < 20 || limit > 512) {
          throw new Error('Invalid negotiated Bluetooth payload limit. Reconnect before retrying.');
        }
        this.fragmentBytes = limit;
      }
      return this;
    } catch (error) {
      this.disconnect(error);
      throw error;
    }
  }

  run(encode) {
    const result = this.queue.then(() => {
      if (this.closed || !this.device.gatt.connected) {
        throw new Error('Bluetooth is disconnected. Reconnect before retrying.');
      }
      const correlation = ++this.sequence;
      return this.exchange(encode(correlation), correlation);
    });
    this.queue = result.catch(() => {});
    return result;
  }

  async exchange(payload, correlation) {
    const frame = encodeFrame(BLE_API.COMMAND, payload);
    let resolveReply;
    const response = new Promise((resolve) => {
      resolveReply = resolve;
    });
    const interrupted = new Promise((_resolve, reject) => {
      const timer = setTimeout(() => {
        this.disconnect(
          new Error(
            'Bluetooth request timed out. Operation outcome may be unknown; inspect the target before retrying.'
          )
        );
      }, this.timeoutMs);
      // Notifications can arrive before the browser's final write callback.
      // Keep cancellation and the deadline armed until both have completed.
      this.pending = { correlation, resolve: resolveReply, reject, timer, responded: false };
    });
    const write = async () => {
      // The firmware reports the effective limit for this connection. Older
      // firmware keeps conservative 20-byte fragments; never probe by replay.
      for (let offset = 0; offset < frame.length; offset += this.fragmentBytes) {
        if (this.closed) {
          throw new Error('Bluetooth disconnected during transmission');
        }
        const bytes = frame.slice(offset, offset + this.fragmentBytes);
        if (this.writeWithoutResponse) {
          await this.rx.writeValueWithoutResponse(bytes);
        } else {
          await this.rx.writeValueWithResponse(bytes);
        }
      }
    };
    try {
      const [reply] = await Promise.race([Promise.all([response, write()]), interrupted]);
      return reply;
    } catch (error) {
      this.disconnect(error);
      throw error;
    } finally {
      if (this.pending?.correlation === correlation) {
        clearTimeout(this.pending.timer);
        this.pending = null;
      }
    }
  }

  async request(path, data) {
    const reply = await this.run((correlation) => encodeRequest(path, data, correlation));
    if (reply.status < 200 || reply.status >= 300) {
      throw new HttpError(reply.status);
    }
    return reply.json ? JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(reply.data)) : reply.data;
  }

  disconnect(reason = new Error('Bluetooth disconnected')) {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.device.removeEventListener('gattserverdisconnected', this.disconnected);
    this.tx.removeEventListener('characteristicvaluechanged', this.received);
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.reject(reason);
      this.pending = null;
    }
    if (this.device.gatt.connected) {
      this.device.gatt.disconnect();
    }
    this.releaseLock?.();
    this.onDisconnect(reason);
  }
}

export async function connectBluetooth({
  bluetooth = globalThis.navigator?.bluetooth,
  locks = globalThis.navigator?.locks,
  onDisconnect,
  connectTimeoutMs = 30000
} = {}) {
  if (!bluetooth || !globalThis.isSecureContext) {
    throw new Error('Bluetooth requires a compatible browser and HTTPS, or localhost for development.');
  }
  // requestDevice must run from the click handler before any asynchronous work.
  const device = await bluetooth.requestDevice({
    filters: [{ namePrefix: 'ChameleonUltra', services: [NORDIC_UART.service] }]
  });
  let releaseLock;
  if (locks) {
    const released = new Promise((resolve) => {
      releaseLock = resolve;
    });
    const acquired = new Promise((resolve, reject) => {
      locks
        .request(`chameleon-bluetooth:${device.id}`, { ifAvailable: true }, async (lock) => {
          resolve(!!lock);
          if (lock) {
            await released;
          }
        })
        .catch(reject);
    });
    if (!(await acquired)) {
      throw new Error('This Ultra is already open in another tab. Close its Bluetooth connection first.');
    }
  }
  let transport;
  let cancelled = false;
  let timer;
  const timeoutError = new Error('Bluetooth connection timed out. Wake the Ultra and reconnect.');
  const step = async (promise) => {
    const result = await promise;
    // GATT operations cannot be aborted. A late connection must not acquire
    // the radio after the UI has already reported a timeout.
    if (cancelled) {
      if (device.gatt.connected) {
        device.gatt.disconnect();
      }
      throw timeoutError;
    }
    return result;
  };
  try {
    const setup = async () => {
      const server = await step(device.gatt.connect());
      const service = await step(server.getPrimaryService(NORDIC_UART.service));
      const rx = await step(service.getCharacteristic(NORDIC_UART.rx));
      const tx = await step(service.getCharacteristic(NORDIC_UART.tx));
      transport = new BluetoothTransport(device, rx, tx, { onDisconnect });
      transport.releaseLock = releaseLock;
      return step(transport.initialize());
    };
    return await Promise.race([
      setup(),
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(timeoutError), connectTimeoutMs);
      })
    ]);
  } catch (error) {
    cancelled = true;
    transport?.disconnect(error);
    if (device.gatt.connected) {
      device.gatt.disconnect();
    }
    releaseLock?.();
    throw error;
  } finally {
    clearTimeout(timer);
  }
}
