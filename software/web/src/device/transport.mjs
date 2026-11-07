// @ts-check
import { HOSTED } from '../runtime/environment.mjs';
import { decodeClassicJob, jobIdBytes } from './classic-job.mjs';

/** @param {Iterable<number> | ArrayLike<number>} bytes */
export const hex = (bytes) =>
  Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0'))
    .join('')
    .toUpperCase();

export class HttpError extends Error {
  /** @param {number} httpStatus */
  constructor(httpStatus) {
    super(
      httpStatus === 409
        ? 'Device busy, session expired or slot changed; refresh before retrying'
        : `Device HTTP ${httpStatus}`
    );
    this.httpStatus = httpStatus;
  }
}

/** @type {{request(path: string, data?: Uint8Array<ArrayBuffer>): Promise<any>,
 * classicJob?: (lease: number, body: any, options: any) => Promise<any>,
 * capabilities?: {classicJobs?: boolean}} | null} */
let deviceTransport = HOSTED
  ? {
      async request() {
        throw new Error('Connect your Ultra with the Bluetooth button.');
      }
    }
  : null;

/** @param {typeof deviceTransport} transport */
export function setDeviceTransport(transport) {
  deviceTransport = transport;
}

export const deviceTransportType = () => (deviceTransport ? 'ble' : 'usb');

/** @param {string} path @param {Uint8Array<ArrayBuffer>} [data] */
export async function request(path, data) {
  if (deviceTransport) {
    return deviceTransport.request(path, data);
  }
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 20000);

  try {
    const response = await fetch(path, {
      method: data === undefined ? 'GET' : 'POST',
      body: data,
      headers: data === undefined ? {} : { 'Content-Type': 'application/octet-stream' },
      cache: 'no-store',
      signal: controller.signal
    });

    if (!response.ok) {
      throw new HttpError(response.status);
    }

    const bytes = new Uint8Array(await response.arrayBuffer());

    if (response.headers.get('Content-Type')?.includes('application/json')) {
      return JSON.parse(new TextDecoder().decode(bytes));
    }

    return bytes;
  } finally {
    clearTimeout(timeout);
  }
}

/** @param {string} path @param {Uint8Array<ArrayBuffer>} [data] */
export const post = (path, data = new Uint8Array()) => request(path, data);

// Pin the native backend for a job; disconnect must never switch it to HTTP.
export function nativeClassicJobRunner() {
  const backend = deviceTransport;
  return backend?.capabilities?.classicJobs && backend.classicJob ? backend.classicJob.bind(backend) : null;
}

/** @param {number} lease @param {any} body
 * @param {{signal?: AbortSignal, onEvent?: (event: any) => void}} options */
export async function classicJob(lease, body, { signal, onEvent = () => {} } = {}) {
  if (deviceTransport) {
    if (!deviceTransport.classicJob) {
      throw new Error('Classic jobs are unavailable on this transport');
    }
    return deviceTransport.classicJob(lease, body, { signal, onEvent });
  }
  if (signal?.aborted) {
    throw new Error('Operation cancelled');
  }
  const started = await post(`/api/classic/job/start?id=${lease}`, Uint8Array.from(body));
  if (started.length !== 4) {
    throw new Error('Invalid Classic job start reply');
  }
  const id = new DataView(started.buffer, started.byteOffset, started.byteLength).getUint32(0);
  if (!id) {
    throw new Error('Invalid Classic job identifier');
  }
  let sequence = 0;
  let cancelled = false;
  let updated = Date.now();
  let previous = 0;
  for (;;) {
    if (signal?.aborted && !cancelled) {
      await post(`/api/classic/job/cancel?id=${lease}`, jobIdBytes(id));
      cancelled = true;
    }
    const bytes = await post(`/api/classic/job/read?id=${lease}`, jobIdBytes(id));
    if (!bytes.length) {
      if (Date.now() - updated > 20000) {
        throw new Error('Classic job stopped responding');
      }
      await new Promise((resolve) => setTimeout(resolve, 5));
      continue;
    }
    const event = decodeClassicJob(bytes, id, sequence++, body[1], previous);
    previous = event.completed;
    updated = Date.now();
    onEvent(event);
    if (event.done) {
      return event;
    }
  }
}

export class DeviceSession {
  /** @param {AbortSignal} [signal] */
  constructor(signal) {
    this.signal = signal;
    this.session = 0;
    /** @type {HttpError | null} */
    this.error = null;
    /** @type {ReturnType<typeof setInterval> | undefined} */
    this.timer = undefined;
    this.pinging = false;
  }

  check() {
    if (this.signal?.aborted) {
      throw new Error('Operation cancelled');
    }
    if (this.error) {
      throw this.error;
    }
  }

  async reserve(path = '/api/session') {
    this.check();
    if (this.session) {
      throw new Error('Device session already open');
    }

    const { session } = await post(path);

    if (!Number.isInteger(session) || session <= 0) {
      throw new Error('Invalid device reservation');
    }

    this.session = session;
    this.timer = setInterval(() => {
      if (this.pinging) {
        return;
      }

      this.pinging = true;
      post(`/api/keepalive?id=${session}`)
        .catch((error) => {
          if (error instanceof HttpError && this.session === session) {
            this.error = error;
          }
        })
        .finally(() => {
          this.pinging = false;
        });
    }, 4000);

    this.check();
  }

  async end() {
    clearInterval(this.timer);
    this.timer = undefined;

    const session = this.session;
    this.session = 0;

    if (session) {
      await post(`/api/end?id=${session}`);
    }
  }
}

/** @template T @param {{end(): Promise<void>}} device @param {() => Promise<T>} work */
export async function withDevice(device, work) {
  let failed = false;

  try {
    return await work();
  } catch (error) {
    failed = true;
    throw error;
  } finally {
    try {
      await device.end();
    } catch (error) {
      if (!failed) {
        throw error;
      }
    }
  }
}
