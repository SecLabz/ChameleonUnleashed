import test from 'node:test';
import assert from 'node:assert/strict';
import { deviceViews } from '../src/ui/device-view.mjs';
import { slotView } from '../src/ui/slots.mjs';
import { BluetoothTransport } from '../src/device/bluetooth.mjs';
import { request, setDeviceTransport } from '../src/device/transport.mjs';

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

    insertAdjacentHTML() {},
    querySelectorAll: () => []
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

test('the connection card, pill and Scan prompt share one USB connection state', async (t) => {
  const get = dom(t);
  let fail = true;
  t.mock.method(globalThis, 'fetch', async (path) => {
    if (fail) {
      throw new Error('Failed to fetch');
    }
    return response(path);
  });

  const views = deviceViews({ notify() {}, onConnection() {} });
  assert.equal(get('connection-title').textContent, 'Connecting…');
  assert.equal(get('connection').dataset.state, 'pending');
  assert.equal(get('scan-connect').hidden, true, 'Scan waits for the first check');

  await views.refresh();
  assert.equal(get('connection-title').textContent, 'Ultra not reachable');
  assert.equal(get('connection').textContent, 'Not connected');
  assert.equal(get('connection').dataset.state, 'offline');
  assert.equal(get('scan-connect').hidden, false);
  assert.equal(get('scanner').hidden, true);
  assert.equal(get('scan-connect-button').textContent, 'Retry');
  assert.equal(get('device-settings').hidden, true, 'stale settings are not editable');
  assert.equal(get('emulation-state').hidden, true);
  assert.equal(get('connect-bluetooth').hidden, true, 'the USB-served app has no Web Bluetooth');

  fail = false;
  await views.refresh();
  assert.equal(get('connection-title').textContent, 'Chameleon Ultra');
  assert.equal(get('connection-help').textContent, 'Connected over USB');
  assert.equal(get('connection').dataset.state, 'connected');
  assert.equal(get('scan-connect').hidden, true);
  assert.equal(get('scanner').hidden, false);
  assert.equal(get('device-settings').hidden, false);
  assert.equal(get('connection-actions').hidden, true, 'USB alone offers no transport actions');
  assert.equal(get('connection-tools').hidden, true, 'nor controls beside the connection status');
  assert.equal(get('firmware-update-note').hidden, true, 'firmware updates are available over USB');
});

test('an aborted firmware reconnect disconnects a late transport without replacing the backend', async (t) => {
  dom(t);
  const original = new Map(
    ['navigator', 'isSecureContext'].map((name) => [name, Object.getOwnPropertyDescriptor(globalThis, name)])
  );
  const peripheral = new EventTarget();
  const characteristic = new EventTarget();
  let disconnects = 0;
  peripheral.gatt = {
    connected: true,
    connect: async function () {
      return this;
    },
    getPrimaryService: async () => ({ getCharacteristic: async () => characteristic }),
    disconnect() {
      this.connected = false;
      disconnects++;
      peripheral.dispatchEvent(new Event('gattserverdisconnected'));
    }
  };
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: { bluetooth: { requestDevice: async () => peripheral } }
  });
  Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
  setDeviceTransport({ request: async () => 'original backend' });
  t.after(() => {
    setDeviceTransport(null);
    for (const [name, descriptor] of original) {
      if (descriptor) {
        Object.defineProperty(globalThis, name, descriptor);
      } else {
        delete globalThis[name];
      }
    }
  });
  let release;
  let reached;
  const waiting = new Promise((resolve) => (release = resolve));
  const initializing = new Promise((resolve) => (reached = resolve));
  t.mock.method(BluetoothTransport.prototype, 'initialize', async function () {
    reached();
    await waiting;
    return this;
  });
  const views = deviceViews({ notify() {}, onConnection() {}, canConnect: () => false });
  const controller = new AbortController();
  const cancelled = new Error('Update closed');
  const connecting = views.connect(true, controller.signal);
  const rejected = assert.rejects(connecting, (error) => error === cancelled);
  await initializing;
  controller.abort(cancelled);
  release();
  await rejected;
  assert.equal(disconnects, 1);
  assert.equal(await request('/api/status'), 'original backend');
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
