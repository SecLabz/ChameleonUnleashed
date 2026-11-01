import test from 'node:test';
import assert from 'node:assert/strict';
import { tearoffDialog } from '../src/ui/st25tb-dialog.mjs';

const uid = [0xd0, 2, 0x33, 1, 2, 3, 4, 5];
const item = { block: 5, after: [0x78, 0x56, 0x34, 0x12] };

const done = () => ({
  state: 3,
  outcome: 0,
  block: 5,
  flags: 3,
  current: 0x12345678,
  target: 0x12345678,
  attempts: 9,
  delay_us: 130,
  seconds: 2
});

function elements() {
  const nodes = new Map();
  globalThis.document = {
    getElementById(id) {
      if (!nodes.has(id)) {
        const classes = new Set();
        nodes.set(id, {
          hidden: true,
          disabled: false,
          textContent: '',
          onclick: null,
          classes,
          classList: {
            toggle(name, value) {
              if (value) {
                classes.add(name);
              } else {
                classes.delete(name);
              }
            }
          }
        });
      }

      return nodes.get(id);
    }
  };

  return (id) => document.getElementById(id);
}

test('dialog renders the terminal result, closes, and removes its abort listener', async () => {
  const previous = globalThis.document;

  try {
    const $ = elements();
    const controller = new AbortController();
    const starts = [];
    let cancellations = 0;
    await tearoffDialog(
      {
        async tearoffStart(...args) {
          starts.push(args);
        },

        async tearoffState() {
          return done();
        },

        async tearoffCancel() {
          cancellations++;
        }
      },
      uid,
      item,
      controller.signal
    );

    assert.deepEqual(starts, [[uid, 5, 0x12345678]]);
    assert.equal($('st25-tear-current').textContent, '12345678');
    assert.equal($('st25-tear-target').textContent, '12345678');
    assert.equal($('st25-tear-attempts').textContent, '9');
    assert.equal($('st25-tear-timing').textContent, '130 µs');
    assert.equal($('st25-tear-duration').textContent, '2 s');
    assert.equal($('st25-tear-progress').classes.has('complete'), true);
    assert.equal($('st25-tearoff').hidden, true);
    assert.equal($('st25-tear-cancel').onclick, null);

    controller.abort();

    assert.equal(cancellations, 0);
  } finally {
    globalThis.document = previous;
  }
});

test('dialog rejects unsuccessful, unclean and mismatched terminal results and still closes', async () => {
  const previous = globalThis.document;

  try {
    const $ = elements();
    for (const change of [{ outcome: 1 }, { flags: 1 }, { current: 0 }]) {
      await assert.rejects(
        tearoffDialog(
          {
            async tearoffStart() {},

            async tearoffState() {
              return { ...done(), ...change };
            }
          },
          uid,
          item
        ),
        /reread before retrying/
      );
      assert.equal($('st25-tearoff').hidden, true);
      assert.equal($('st25-tear-cancel').onclick, null);
    }
  } finally {
    globalThis.document = previous;
  }
});

test('abort requests cancellation once, a failed cancellation permits retry, and polling stays bounded', async () => {
  const previous = globalThis.document;
  const timeout = globalThis.setTimeout;

  try {
    const $ = elements();
    const controller = new AbortController();
    const delays = [];
    let finish;
    let waiting;
    let reads = 0;
    let cancellations = 0;
    const polling = new Promise((resolve) => {
      waiting = resolve;
    });
    globalThis.setTimeout = (callback, delay) => {
      delays.push(delay);
      queueMicrotask(callback);
    };

    const pending = tearoffDialog(
      {
        async tearoffStart() {},

        async tearoffState() {
          reads++;
          if (reads === 1) {
            return { ...done(), state: 2, flags: 0 };
          }
          waiting();

          return new Promise((resolve) => {
            finish = resolve;
          });
        },

        async tearoffCancel() {
          cancellations++;
          if (cancellations === 1) {
            throw new Error('Connection interrupted');
          }
        }
      },
      uid,
      item,
      controller.signal
    );
    await polling;

    assert.deepEqual(delays, [1000]);
    assert.equal($('st25-tear-current').textContent, '—');

    controller.abort();
    $('st25-tear-cancel').onclick();

    assert.equal(cancellations, 1);

    await new Promise(queueMicrotask);

    assert.equal($('st25-tear-cancel').disabled, false);
    assert.equal($('st25-tear-cancel').textContent, 'Retry cancel');

    $('st25-tear-cancel').onclick();

    assert.equal(cancellations, 2);
    assert.equal($('st25-tear-cancel').disabled, true);

    finish({ ...done(), outcome: 1 });

    await assert.rejects(pending, /outcome 1/);
    assert.equal($('st25-tearoff').hidden, true);
    assert.equal($('st25-tear-cancel').onclick, null);
  } finally {
    globalThis.document = previous;
    globalThis.setTimeout = timeout;
  }
});

test('a pending stop or a failed state request closes the dialog', async () => {
  const previous = globalThis.document;

  try {
    const $ = elements();
    const controller = new AbortController();
    controller.abort();

    await assert.rejects(
      tearoffDialog(
        {
          async tearoffStart() {
            assert.fail('Stopped before submission');
          }
        },
        uid,
        item,
        controller.signal
      ),
      /stopped before tearoff/
    );
    await assert.rejects(
      tearoffDialog(
        {
          async tearoffStart() {},

          async tearoffState() {
            throw new Error('Disconnected');
          }
        },
        uid,
        item
      ),
      /Disconnected/
    );
    assert.equal($('st25-tearoff').hidden, true);
    assert.equal($('st25-tear-cancel').onclick, null);
  } finally {
    globalThis.document = previous;
  }
});
