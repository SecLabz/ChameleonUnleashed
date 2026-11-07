// One bounded firmware acquisition job. Records stay ordered and local to this
// recovery; stopping waits for the terminal event and the cancellation reply.
export function createNonceStream({ run, lease, body, budget, signal, onProgress = () => {} }) {
  const controller = new AbortController();
  const records = [];
  const waiters = new Set();
  let received = 0;
  let settled = false;
  let failure = null;
  const wake = () => {
    for (const resolve of waiters) {
      resolve();
    }
    waiters.clear();
  };
  const abort = () => controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) {
    abort();
  }
  const done = Promise.resolve()
    .then(() => {
      if (controller.signal.aborted) {
        throw new Error('Operation cancelled');
      }
      return run(lease, body, {
        signal: controller.signal,
        onEvent(event) {
          if (event.status !== 0) {
            const messages = {
              6: 'Authentication rejected during acquisition',
              1: 'Card not found or changed during acquisition'
            };
            const error = new Error(messages[event.status] ?? `Acquisition status ${event.status}`);
            error.status = event.status;
            failure = error;
            wake();
            return;
          }
          const count = event.data.length / 12;
          if (
            !Number.isInteger(count) ||
            count > 16 ||
            received + count > budget ||
            event.completed !== received + count
          ) {
            throw new Error('Invalid streamed observation count');
          }
          const bytes = event.data;
          const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
          for (let i = 0; i < count; i++) {
            const offset = i * 12;
            const parity = Array.from(bytes.slice(offset + 8, offset + 12));
            if (parity.some((bit) => bit > 1)) {
              throw new Error('Invalid streamed observation parity');
            }
            records.push({ nt1: view.getUint32(offset), ciphertext: view.getUint32(offset + 4), parity });
          }
          received += count;
          onProgress(received);
          wake();
        }
      });
    })
    .then((result) => {
      if (failure) {
        throw failure;
      }
      if (!result.cancelled && received !== budget) {
        throw new Error('Incomplete nonce acquisition job');
      }
      return result;
    })
    .catch((error) => {
      failure = error;
      throw error;
    })
    .finally(() => {
      settled = true;
      signal?.removeEventListener('abort', abort);
      wake();
    });
  done.catch(() => {});
  return {
    get error() {
      return failure;
    },
    get buffered() {
      return records.length;
    },
    async read() {
      while (!records.length && !settled && !failure) {
        await new Promise((resolve) => waiters.add(resolve));
      }
      if (failure) {
        throw failure;
      }
      return records.splice(0, 16);
    },
    async stop() {
      if (!settled) {
        controller.abort();
      }
      await done;
    },
    drain() {
      return records.splice(0);
    }
  };
}
