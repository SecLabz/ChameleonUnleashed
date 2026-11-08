import { HOSTED, SHELL_VERSION, appUrl } from './environment.mjs';

let registration;

async function registered() {
  if (!HOSTED || !SHELL_VERSION || !globalThis.isSecureContext || !navigator.serviceWorker) {
    throw new Error('Offline app storage requires HTTPS and service workers.');
  }
  if (!registration) {
    registration = navigator.serviceWorker
      .register(appUrl('sw.mjs'), {
        type: 'module',
        scope: './',
        updateViaCache: 'none'
      })
      .catch((error) => {
        registration = null;
        throw error;
      });
  }
  return registration;
}

async function inspect(worker, prepare = false) {
  if (!worker) {
    return false;
  }
  const channel = new MessageChannel();
  try {
    return await new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error('Offline app cache did not respond.')),
        prepare ? 15000 : 5000
      );
      channel.port1.onmessage = ({ data }) => {
        clearTimeout(timer);
        resolve(data.version === SHELL_VERSION && data.stored === true);
      };
      worker.postMessage({ type: prepare ? 'shell-prepare' : 'shell-state' }, [channel.port2]);
    });
  } finally {
    channel.port1.close();
    channel.port2.close();
  }
}

export async function shellState() {
  if (!HOSTED) {
    return [];
  }
  const available = !!globalThis.isSecureContext && !!navigator.serviceWorker;
  let stored = false;
  try {
    const reg = await registered();
    stored = await inspect(reg.active);
  } catch {
    // The explicit Retry action reports the installation error; never claim
    // Ready offline when the browser cannot retain the complete app shell.
  }
  return [{ shell: true, asset: { size: 0 }, available, stored, session: false }];
}

export async function prepareShell(signal) {
  const reg = await registered();
  await new Promise((resolve, reject) => {
    const worker = reg.installing || reg.waiting || reg.active;
    const finish = (error) => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', stop);
      worker?.removeEventListener('statechange', changed);
      error ? reject(error) : resolve();
    };
    const stop = () => finish(new Error('Operation cancelled'));
    const changed = () => {
      if (reg.active?.state === 'activated') {
        finish();
      } else if (!worker || worker.state === 'redundant') {
        finish(new Error('The complete app could not be saved. Connect to the Internet and retry.'));
      }
    };
    const timer = setTimeout(
      () => finish(new Error('Offline app preparation timed out. Retry when online.')),
      15000
    );
    signal?.addEventListener('abort', stop, { once: true });
    worker?.addEventListener('statechange', changed);
    signal?.aborted ? stop() : changed();
  });
  if (!(await inspect(reg.active)) && !(await inspect(reg.active, true))) {
    throw new Error('The complete app could not be saved. Connect to the Internet and retry.');
  }
}
