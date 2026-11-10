import test from 'node:test';
import assert from 'node:assert/strict';
import { recoverHardnested } from '../src/recovery/hardnested.mjs';
import { createNonceStream } from '../src/device/nonce-stream.mjs';

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

test('Hardnested drains buffered USB pages before starting another capture', { timeout: 1000 }, async () => {
  let captured = 0;
  let processed = 0;
  let releaseFirstPush;
  const firstPush = new Promise((resolve) => {
    releaseFirstPush = resolve;
  });
  const result = await recoverHardnested({
    device: {
      async command() {},
      async collectNonces(_source, _target, count) {
        const records = Array.from({ length: count }, () => ({ ciphertext: captured++ }));
        if (captured === 48) {
          releaseFirstPush();
        }
        return records;
      }
    },
    source: {},
    target: { sector: 0, is_key_b: false },
    geometry: { sectors: [{ first: 0 }] },
    uid: [1, 2, 3, 4],
    budget: 96,
    check() {},
    update() {},
    verify: async (_target, [key]) => key,
    async call({ op, observations }) {
      if (op === 'hardnested_new') {
        return { job: 1 };
      }
      if (op === 'hardnested_push') {
        if (!processed) {
          await firstPush;
        }
        if (processed === 32) {
          assert.equal(captured, 48, 'the Worker should catch up without requesting an extra RF batch');
        }
        processed += observations.length;
        return { count: processed, diverse: true, covered: 256, early: processed === 48 };
      }
      if (op === 'hardnested_early') {
        return { key: '010203040506' };
      }
      assert.equal(op, 'hardnested_dispose');
      return {};
    }
  });
  assert.deepEqual(result, { Verified: [1, 2, 3, 4, 5, 6] });
});

function fixture({ workerFailure = false, rfFailure = false, abort = false, streaming = false } = {}) {
  const controller = new AbortController();
  const state = {
    captured: 0,
    pushed: [],
    inFlight: 0,
    workerInFlight: 0,
    closed: false,
    disposed: false,
    rejected: false,
    verified: 0,
    checks: []
  };
  const check = () => {
    if (controller.signal.aborted) {
      throw new Error('Operation cancelled');
    }
  };
  const device = {
    async command(command, [open]) {
      assert.equal(command, 2026);
      assert.equal(state.inFlight, 0);

      state.closed = !open;
    },

    async collectNonces(source, target, count) {
      assert.equal(state.inFlight, 0);

      state.inFlight++;

      try {
        await delay(2);
        if (rfFailure && state.captured >= 16) {
          throw new Error('RF failure');
        }

        return Array.from({ length: count }, () => ({
          nt1: 0,
          ciphertext: state.captured++,
          parity: [0, 0, 0, 0]
        }));
      } finally {
        state.inFlight--;
      }
    }
  };
  if (streaming) {
    device.startNonceStream = (source, target, budget) => {
      state.closed = false;
      return createNonceStream({
        budget,
        signal: controller.signal,
        run: async (lease, body, { signal, onEvent }) => {
          state.inFlight++;
          let completed = 0;
          try {
            while (completed < budget && !signal.aborted) {
              await delay(2);
              if (rfFailure && state.captured >= 16) {
                throw new Error('RF failure');
              }
              const count = Math.min(16, budget - completed);
              const data = new Uint8Array(count * 12);
              const view = new DataView(data.buffer);
              for (let i = 0; i < count; i++) {
                view.setUint32(i * 12 + 4, state.captured++);
              }
              completed += count;
              onEvent({ status: 0, completed, data });
            }
            return { cancelled: signal.aborted };
          } finally {
            state.inFlight--;
            state.closed = true;
          }
        }
      });
    };
  }
  const call = async (request) => {
    if (request.op === 'hardnested_dispose') {
      state.disposed = true;

      return { disposed: true };
    }
    check();

    assert.equal(state.workerInFlight, 0);

    state.workerInFlight++;

    try {
      if (request.op === 'hardnested_new') {
        return { job: 1 };
      }

      if (request.op === 'hardnested_push') {
        await delay(workerFailure || rfFailure || abort ? 10 : 1);
        if (abort) {
          controller.abort();
          check();
        }

        if (workerFailure) {
          throw new Error('Worker failure');
        }
        state.pushed.push(...request.observations.map((record) => record.ciphertext));

        return { count: state.pushed.length, covered: 256, diverse: true, early: state.pushed.length === 16 };
      }

      if (request.op === 'hardnested_early') {
        await delay(5);

        return { key: '000000000000' };
      }

      if (request.op === 'hardnested_reject') {
        state.rejected = true;

        return {};
      }

      if (request.op === 'hardnested_finish') {
        assert.equal(state.pushed.length, 64);

        return { key: '010203040506' };
      }
      throw new Error('Unexpected request');
    } finally {
      state.workerInFlight--;
    }
  };
  const verify = async (target, [key]) => {
    assert.equal(state.inFlight, 0);
    assert(state.closed);
    assert(state.pushed.length <= state.captured);

    state.checks.push({ processed: state.pushed.length, captured: state.captured });

    state.verified++;

    return state.verified === 1 ? null : key;
  };
  const run = () =>
    recoverHardnested({
      call,
      device,
      source: {},
      target: { sector: 1, is_key_b: true },
      geometry: { sectors: [{}, { first: 4 }] },
      uid: [1, 2, 3, 4],
      budget: 64,
      verify,
      check,

      update() {}
    });

  return { state, run };
}

for (const streaming of [false, true]) {
  test(`early rejection retains all observations (streaming=${streaming})`, async () => {
    const { state, run } = fixture({ streaming });

    assert.deepEqual(await run(), { Verified: [1, 2, 3, 4, 5, 6] });
    assert(state.rejected);
    assert.equal(state.verified, 2);
    assert(state.checks[0].processed < state.checks[0].captured);
    assert.deepEqual(
      state.pushed,
      Array.from({ length: 64 }, (_, i) => i)
    );
    assert(state.closed && state.disposed);
  });
}

for (const streaming of [false, true]) {
  for (const diverse of [true, false]) {
    test(`reselection processes fresh pages before judging diversity (${diverse}, streaming=${streaming})`, async () => {
      let captured = 0;
      let opens = 0;
      let reselectedAt;
      let releaseFirst;
      let closed = false;
      let disposed = false;
      const pushed = [];
      const device = {
        async command(command, [open]) {
          assert.equal(command, 2026);

          closed = !open;
          if (open) {
            opens++;
            if (opens === 2) {
              reselectedAt = captured;
            }
          }
        },

        async collectNonces(source, target, count) {
          const records = Array.from({ length: count }, () => ({
            nt1: captured++,
            ciphertext: +(diverse && opens > 1),
            parity: [0, 0, 0, 0]
          }));
          if (captured === 48) {
            releaseFirst();
            await delay(0);
          }

          return records;
        }
      };
      if (streaming) {
        device.startNonceStream = (source, target, budget) => {
          opens++;
          closed = false;
          if (opens === 2) {
            reselectedAt = captured;
          }
          return createNonceStream({
            budget,
            run: async (_, __, { signal, onEvent }) => {
              let completed = 0;
              try {
                while (completed < budget && !signal.aborted) {
                  await delay(2);
                  const count = Math.min(16, budget - completed);
                  const data = new Uint8Array(count * 12);
                  const view = new DataView(data.buffer);
                  for (let i = 0; i < count; i++) {
                    view.setUint32(i * 12, captured++);
                    view.setUint32(i * 12 + 4, +(diverse && opens > 1));
                  }
                  completed += count;
                  onEvent({ status: 0, completed, data });
                  if (captured === 48) {
                    releaseFirst();
                  }
                }
                return { cancelled: signal.aborted };
              } finally {
                closed = true;
              }
            }
          });
        };
      }
      const call = async (request) => {
        if (request.op === 'hardnested_new') {
          return { job: 1 };
        }

        if (request.op === 'hardnested_dispose') {
          disposed = true;

          return {};
        }

        if (request.op === 'hardnested_push') {
          if (!pushed.length) {
            await new Promise((resolve) => {
              releaseFirst = resolve;
            });
          }
          pushed.push(...request.observations);

          return { count: pushed.length, diverse: pushed.some((record) => record.ciphertext), covered: 256 };
        }

        assert.equal(request.op, 'hardnested_finish');

        return { key: '010203040506' };
      };
      const result = await recoverHardnested({
        call,
        device,
        source: {},
        target: { sector: 1, is_key_b: true },
        geometry: { sectors: [{}, { first: 4 }] },
        uid: [1, 2, 3, 4],
        budget: 128,
        verify: async (target, [key]) => {
          assert(closed);

          return key;
        },

        check() {},

        update() {}
      });

      assert.equal(opens, 2);
      assert(pushed.length > reselectedAt);
      assert.deepEqual(
        pushed.map((record) => record.nt1),
        Array.from({ length: pushed.length }, (_, i) => i)
      );
      assert.deepEqual(result, diverse ? { Verified: [1, 2, 3, 4, 5, 6] } : 'NoDiversity');

      if (diverse) {
        assert.equal(pushed.length, 128);
      }

      assert(closed && disposed);
    });
  }
}

for (const streaming of [false, true]) {
  for (const [name, options, message] of [
    ['Worker failure', { workerFailure: true }, /Worker failure/],
    ['RF failure', { rfFailure: true }, /RF failure/],
    ['Stop', { abort: true }, /cancelled/]
  ]) {
    test(`${name} settles RF and Worker before cleanup (streaming=${streaming})`, async () => {
      const { state, run } = fixture({ ...options, streaming });

      await assert.rejects(run(), message);
      assert.equal(state.inFlight, 0);
      assert.equal(state.workerInFlight, 0);
      assert.equal(state.verified, 0);
      assert(state.closed && state.disposed);
    });
  }
}
