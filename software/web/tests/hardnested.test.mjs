import test from 'node:test';
import assert from 'node:assert/strict';
import { recoverHardnested } from '../src/recovery/hardnested.mjs';

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function fixture({ workerFailure = false, rfFailure = false, abort = false } = {}) {
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

test('early rejection retains overlapped pages and completes the full budget', async () => {
  const { state, run } = fixture();

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

for (const diverse of [true, false]) {
  test(`reselection processes fresh pages before judging diversity (${diverse})`, async () => {
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

for (const [name, options, message] of [
  ['Worker failure', { workerFailure: true }, /Worker failure/],
  ['RF failure', { rfFailure: true }, /RF failure/],
  ['Stop', { abort: true }, /cancelled/]
]) {
  test(`${name} settles RF and Worker before cleanup`, async () => {
    const { state, run } = fixture(options);

    await assert.rejects(run(), message);
    assert.equal(state.inFlight, 0);
    assert.equal(state.workerInFlight, 0);
    assert.equal(state.verified, 0);
    assert(state.closed && state.disposed);
  });
}
