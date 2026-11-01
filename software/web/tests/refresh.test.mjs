import test from 'node:test';
import assert from 'node:assert/strict';
import { deviceViews } from '../src/ui/device-view.mjs';
import { slotView } from '../src/ui/slots.mjs';

function dom(t) {
  const previous = globalThis.document;
  const elements = new Map();
  const node = () => ({
    children: [],
    dataset: {},

    setAttribute() {},

    removeAttribute() {},

    append(...children) {
      this.children.push(...children);
    },

    replaceChildren(...children) {
      this.children = children;
    },

    insertAdjacentHTML() {}
  });
  const get = (id) => {
    if (!elements.has(id)) {
      elements.set(id, node());
    }

    return elements.get(id);
  };
  globalThis.document = {
    getElementById: get,
    createElement: node,
    querySelectorAll: () => [get('connection'), get('nav-connection')]
  };
  t.after(() => {
    globalThis.document = previous;
  });

  return get;
}

const device = {
  active_slot: 0,
  battery_percent: 87,
  battery_mv: 4012,
  version: '2.2',
  git: 'test',
  chip: 'test',
  settings: {}
};

const response = (path) =>
  path === '/api/device'
    ? new Response(JSON.stringify(device), { headers: { 'Content-Type': 'application/json' } })
    : new Response(new Uint8Array(15));

test('a manual device refresh can await an existing refresh and errors do not prevent a later retry', async (t) => {
  dom(t);

  let release;
  let requests = 0;
  let fail = false;
  const waiting = new Promise((resolve) => {
    release = resolve;
  });
  t.mock.method(globalThis, 'fetch', async (path) => {
    requests++;
    await waiting;
    if (fail) {
      throw new Error('Disconnected');
    }

    return response(path);
  });

  const connections = [];
  const notices = [];
  const views = deviceViews({
    notify: (message) => notices.push(message),
    onConnection: (value) => connections.push(value)
  });
  const first = views.refresh();
  const second = views.refresh();

  assert.equal(first, second);

  release();

  assert.equal((await second).ok, true);
  assert.equal(requests, 2);

  fail = true;

  const failed = await views.refresh();

  assert.equal(failed.ok, false);
  assert.match(failed.error.message, /Disconnected/);

  fail = false;

  assert.equal((await views.refresh()).ok, true);
  assert.deepEqual(connections, [true, false, true]);
  assert.deepEqual(notices, [], 'background refresh stays silent');
});

test('concurrent slot refreshes wait for all eight slots instead of returning premature success', async (t) => {
  const get = dom(t);
  let release;
  let requests = 0;
  const waiting = new Promise((resolve) => {
    release = resolve;
  });
  t.mock.method(globalThis, 'fetch', async (path) => {
    requests++;
    await waiting;

    return response(path);
  });

  const slots = slotView({ notify() {} });
  const first = slots.refresh();
  const second = slots.refresh();

  assert.equal(first, second);
  assert.equal(get('slots').children.length, 0);

  release();
  await second;

  assert.equal(requests, 9);
  assert.equal(get('slots').children.length, 8);
});

test('slot mutations keep workspace ownership and defer feedback until refreshed', async (t) => {
  dom(t);
  const events = [];
  let release;
  let reached;
  const waiting = new Promise((resolve) => {
    release = resolve;
  });
  const refreshing = new Promise((resolve) => {
    reached = resolve;
  });
  t.mock.method(globalThis, 'fetch', async (path) => {
    if (path === '/api/slots/session') {
      return Response.json({ session: 1 });
    }
    if (path === '/api/end?id=1') {
      events.push('device released');
    }
    if (path === '/api/slot/7') {
      reached();
      await waiting;
    }
    return response(path);
  });
  const slots = slotView({
    async run(work) {
      events.push('workspace reserved');
      await work(new AbortController().signal);
      events.push('workspace released');
    },
    afterChange: async () => events.push('device refreshed'),
    notify: (message) => events.push(message)
  });

  const mutation = slots.mutate(async () => {}, 'Saved');
  await refreshing;

  assert.deepEqual(events, ['workspace reserved', 'device released']);

  release();
  await mutation;

  assert.deepEqual(events, [
    'workspace reserved',
    'device released',
    'device refreshed',
    'Saved',
    'workspace released'
  ]);
});

for (const [mutationFails, refreshFails] of [
  [true, false],
  [false, true],
  [true, true]
]) {
  test(`slot mutation failures remain primary and never report success: ${mutationFails}/${refreshFails}`, async (t) => {
    dom(t);
    const mutationError = new Error('Slot write failed');
    const refreshError = new Error('Refresh failed');
    const events = [];
    const notices = [];
    t.mock.method(globalThis, 'fetch', async (path) => {
      if (path === '/api/slots/session') {
        return Response.json({ session: 1 });
      }
      if (path === '/api/end?id=1') {
        events.push('device released');
      }
      if (path === '/api/device') {
        events.push('refresh requested');
        if (refreshFails) {
          throw refreshError;
        }
      }
      return response(path);
    });
    const slots = slotView({
      run: (work) => work(new AbortController().signal),
      afterChange: async () => events.push('device refreshed'),
      notify: (message) => notices.push(message)
    });

    await assert.rejects(
      slots.mutate(async () => {
        if (mutationFails) {
          throw mutationError;
        }
      }, 'Saved'),
      (error) => error === (mutationFails ? mutationError : refreshError)
    );

    assert.equal(events[0], 'device released');
    assert.equal(events[1], 'refresh requested');
    assert.equal(events.includes('device refreshed'), !refreshFails);
    assert.deepEqual(notices, []);
  });
}
