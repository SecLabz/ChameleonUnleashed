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
import { decodeClassicJob, jobIdBytes } from './classic-job.mjs';
import { COMMAND } from './commands.mjs';
import { claimBluetoothDevice } from './bluetooth-lock.mjs';

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
          if (frame.command === BLE_API.JOB_EVENT) {
            if (!this.job || frame.status !== 0x68) {
              throw new Error('Unexpected Classic job event');
            }
            this.job.receive(frame.data);
            continue;
          }
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
        throw new Error(
          'Unsupported Web Bluetooth firmware contract. Update the firmware through Firmware update or USB DFU.'
        );
      }
      const limits = new DataView(hello.data.buffer, hello.data.byteOffset, hello.data.byteLength);
      if (
        hello.data[0] !== BLE_API.VERSION ||
        limits.getUint16(1) < BLE_API.MAX_BODY ||
        limits.getUint16(3) < BLE_API.MAX_RESPONSE
      ) {
        throw new Error(
          'Incompatible Web Bluetooth firmware limits. Update the firmware through Firmware update or USB DFU.'
        );
      }
      this.capabilities = { tearoff: !!(hello.data[5] & 1), classicJobs: !!(hello.data[5] & 4) };
      if (hello.data[5] & 2) {
        const info = await this.run((correlation) => encodeOperation(OPERATION.TRANSPORT_INFO, correlation));
        const limit = info.data.length === 3 ? info.data[1] * 256 + info.data[2] : 0;
        if (info.status !== 200 || info.json || info.data[0] !== 1 || limit < 20 || limit > 512) {
          throw new Error('Invalid negotiated Bluetooth payload limit. Reconnect before retrying.');
        }
        this.fragmentBytes = limit;
      }
      if (hello.data[5] & 8) {
        // The central chooses the effective interval. A declined profile must
        // retain the working connection and must never replay a reader command.
        const profile = await this.run((correlation) =>
          encodeOperation(OPERATION.LINK_PROFILE, correlation, 0, 0, Uint8Array.of(2, 0))
        );
        if (profile.status !== 200 || profile.data.length !== 24 || profile.data[0] !== 1) {
          throw new Error('Invalid Bluetooth link profile response');
        }
        this.linkProfile = profile.data;
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

  async classicJob(lease, body, { signal, onEvent = () => {} } = {}) {
    if (this.job || !this.capabilities?.classicJobs || signal?.aborted) {
      throw new Error('Classic job is unavailable');
    }
    let resolve;
    let reject;
    let timer;
    const finished = new Promise((yes, no) => {
      resolve = yes;
      reject = no;
    });
    // Attach immediately: disconnect can reject while the start exchange is pending.
    finished.catch(() => {});
    const early = [];
    let sequence = 0;
    let previous = 0;
    const arm = () => {
      clearTimeout(timer);
      timer = setTimeout(() => this.disconnect(new Error('Classic job stopped responding')), this.timeoutMs);
    };
    const job = (this.job = {
      id: null,
      reject,
      receive: (bytes) => {
        if (job.id === null) {
          if (early.length >= 4) {
            throw new Error('Classic job start event overflow');
          }
          early.push(bytes);
          return;
        }
        const event = decodeClassicJob(bytes, job.id, sequence++, body[1], previous);
        previous = event.completed;
        arm();
        onEvent(event);
        if (event.done) {
          resolve(event);
        }
      }
    });
    let cancellation;
    const cancel = () => {
      if (job.id !== null && !cancellation) {
        cancellation = this.request(`/api/classic/job/cancel?id=${lease}`, jobIdBytes(job.id));
        cancellation.catch((error) => this.disconnect(error));
      }
    };
    signal?.addEventListener('abort', cancel, { once: true });
    arm();
    try {
      const reply = await this.request(`/api/classic/job/start?id=${lease}`, Uint8Array.from(body));
      if (reply.length !== 4) {
        throw new Error('Invalid Classic job start reply');
      }
      job.id = new DataView(reply.buffer, reply.byteOffset, reply.byteLength).getUint32(0);
      if (!job.id) {
        throw new Error('Invalid Classic job identifier');
      }
      early.splice(0).forEach(job.receive);
      if (signal?.aborted) {
        cancel();
      }
      const result = await finished;
      await cancellation;
      return result;
    } catch (error) {
      this.disconnect(error);
      throw error;
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener('abort', cancel);
      if (this.job === job) {
        this.job = null;
      }
    }
  }

  async enterDfu(signal) {
    await this.queue;
    signal?.throwIfAborted();
    if (this.closed || !this.device.gatt.connected) {
      throw new Error('Reconnect the Ultra before entering DFU.');
    }
    let timer;
    let disconnected;
    let abort;
    const reset = new Promise((resolve, reject) => {
      disconnected = resolve;
      abort = () => reject(signal.reason);
      this.device.addEventListener('gattserverdisconnected', disconnected, { once: true });
      signal?.addEventListener('abort', abort, { once: true });
      timer = setTimeout(() => reject(new Error('The Ultra did not restart in DFU.')), this.timeoutMs);
    });
    // This legacy command resets immediately, without an application reply.
    // A GATT write can reject during that reset; the observed disconnect is required.
    void this.rx.writeValueWithResponse(encodeFrame(COMMAND.ENTER_BOOTLOADER)).catch(() => {});
    try {
      await reset;
    } finally {
      clearTimeout(timer);
      this.device.removeEventListener('gattserverdisconnected', disconnected);
      signal?.removeEventListener('abort', abort);
    }
  }

  disconnect(reason = new Error('Bluetooth disconnected')) {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.job?.reject(reason);
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
  const releaseLock = await claimBluetoothDevice(device, locks);
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
