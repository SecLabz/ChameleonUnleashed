import test from 'node:test';
import assert from 'node:assert/strict';
import { memoryEditor } from '../src/ui/memory-editor.mjs';

function harness(t, state) {
  const previous = globalThis.document;
  globalThis.document = {
    createElement(tag) {
      const attributes = new Map();

      return {
        tag,
        children: [],
        style: { setProperty() {} },
        classList: { toggle() {} },

        setAttribute(key, value) {
          attributes.set(key, value);
        },

        getAttribute: (key) => attributes.get(key),

        append(...children) {
          this.children.push(...children);
        }
      };
    }
  };
  t.after(() => {
    globalThis.document = previous;
  });

  const received = [];
  const [root, message] = memoryEditor(state, { onInput: (parsed) => received.push(parsed) });
  const [gutter, code, ascii] = root.children;
  const [overlay, input] = code.children;

  return {
    root,
    gutter,
    overlay,
    input,
    ascii,
    message,
    received,

    edit(text) {
      input.value = text;
      input.oninput();

      return received.at(-1);
    }
  };
}

const classic = () => ({
  kind: 'classic',
  unitBytes: 16,
  card: { uid: [1, 2, 3, 4] },
  geometry: { sectors: [{ first: 0, count: 4, trailer: 3 }] },
  memory: [
    Array(16).fill(0),
    null,
    Array(16).fill(0),
    [...Array(6).fill(255), 255, 7, 128, 105, ...Array(6).fill(255)]
  ]
});

test('Classic editor retains sparse bytes, separates trailer keys and reports invalid lines', (t) => {
  const state = classic();
  const ui = harness(t, state);

  assert.match(ui.gutter.innerHTML, /S0/);
  assert.match(ui.gutter.innerHTML, /A✓/);
  assert.match(ui.overlay.innerHTML, /t-key-a/);

  const lines = ui.input.value.split('\n');
  lines[1] = '41??0000 00000000 00000000 00000000';

  const parsed = ui.edit(lines.join('\n'));

  assert.deepEqual(parsed[1].slice(0, 3), [65, null, 0]);
  assert.match(ui.ascii.innerHTML, /A\?/);
  assert.equal(state.memory[1], null, 'editor does not mutate the document before the caller commits');

  lines[2] = 'invalid';

  assert.equal(ui.edit(lines.join('\n')), null);
  assert.equal(ui.input.getAttribute('aria-invalid'), 'true');
  assert.match(ui.message.textContent, /Line 3 · block 002/);
  assert.match(ui.gutter.innerHTML, /bad/);
});

test('row count and extra pasted lines never produce a partial commit', (t) => {
  const ui = harness(t, classic());
  const text = ui.input.value;

  assert.equal(ui.edit(text.split('\n').slice(0, 3).join('\n')), null);
  assert.match(ui.message.textContent, /missing/);
  assert.equal(ui.edit(text + '\nAB'), null);
  assert.match(ui.message.textContent, /no block/);
  assert.equal(ui.edit(text + '\n\n').length, 4);
});

test('Classic key indicators follow edits rather than the original recovery', (t) => {
  const state = classic();
  state.recoveredKeys = [{ key_a: Array(6).fill(255), key_b: Array(6).fill(255) }];

  const ui = harness(t, state);
  const lines = ui.input.value.split('\n');
  lines[3] = '???????????? FF078069 010203040506';

  assert(ui.edit(lines.join('\n')));
  assert.match(ui.gutter.innerHTML, /A\?/);
  assert.match(ui.gutter.innerHTML, /B✓/);
  assert.doesNotMatch(ui.gutter.innerHTML, /A✓/);
});

test('ST25TB keeps MSB first words, unknown blocks and separate sysOTP', (t) => {
  const memory = Array.from({ length: 16 }, () => [0, 0, 0, 0]);
  memory[2] = null;
  memory[5] = [254, 255, 255, 255];

  const state = { kind: 'st25tb', type: 2000, memory, card: { system: [255, 255, 255, 255] } };
  const ui = harness(t, state);
  const text = ui.input.value;

  assert.equal(text.split('\n').length, 17);
  assert.equal(text.split('\n')[5], 'FFFFFFFE');
  assert.match(ui.gutter.innerHTML, /sysOTP/);
  assert.equal(ui.edit(text.replace('????????', '12345678')), null);
  assert.match(ui.message.textContent, /unread data cannot be written/);
  assert.equal(ui.edit(text.replace('FFFFFFFF', 'FFFFFFFE'))[16][0], 254);
  assert.equal(ui.edit(text.replace('00000000', '0000000?')), null);
});

test('Type 2 uses the actual Ultralight C profile and rejects partial-byte rows', (t) => {
  const state = {
    kind: 'type2',
    type: 1104,
    unitBytes: 4,
    geometry: { profile: 'ultralight_c' },
    memory: Array(48).fill(null)
  };
  const ui = harness(t, state);

  assert.match(ui.gutter.innerHTML, /Key/);
  assert.match(ui.gutter.innerHTML, /Counter/);
  assert.equal(ui.edit(ui.input.value.replace('????????', '01??????')), null);
  assert.match(ui.message.textContent, /complete hex bytes/);
  assert.equal(ui.edit(ui.input.value.replace('01??????', '01020304'))[0][0], 1);
});
