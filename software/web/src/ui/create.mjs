import { LF_TYPES } from '../documents/lf.mjs';
import { TYPE2_PROFILES, typeName } from '../documents/slot-image.mjs';
import { parseHex } from '../documents/workspace.mjs';
import { $ } from './device-view.mjs';

const GROUPS = [
  ['MIFARE Classic', [1001, 1003, 1000, 1002]],
  ['NTAG / Ultralight', Object.keys(TYPE2_PROFILES).map(Number)],
  ['ICODE', [1501, 1500]],
  ['RFID (LF)', LF_TYPES]
];
const DEFAULTS = [1001, 1100, 1101, 100];
const RECENT_MODELS = 'chameleon-create-models-v1';
const normalize = (text) => text.toLowerCase().replace(/[^a-z0-9]/g, '');

export function createView(onCreate, onCancel) {
  const dialog = $('create-dialog');
  const error = $('create-error');
  const search = $('create-search');
  const all = $('create-all');
  const suggestions = $('create-suggestions');
  const models = $('create-models');
  const catalog = new Map(
    GROUPS.flatMap(([label, types]) =>
      types.map((type) => [
        type,
        normalize(`${label} ${typeName(type)} ${type >= 1000 ? 'NFC HF' : 'RFID LF'}`)
      ])
    )
  );
  let pending = false;
  let recent = [];
  try {
    const saved = JSON.parse(localStorage.getItem(RECENT_MODELS) || '[]');
    if (Array.isArray(saved)) {
      recent = saved.filter((type) => catalog.has(type)).slice(0, 4);
    }
  } catch {
    /* Model history is optional. */
  }

  function showError(message) {
    error.textContent = message;
    error.hidden = false;
    error.scrollIntoView({ block: 'nearest' });
  }

  function close() {
    dialog.close();
  }

  function button(type) {
    const element = document.createElement('button');
    element.type = 'button';
    element.dataset.create = String(type);
    element.textContent = typeName(type);
    element.disabled = pending;
    return element;
  }

  function render() {
    const query = search.value.trim();
    const terms = query.split(/\s+/).map(normalize).filter(Boolean);
    const exact = [...catalog.keys()].find((type) => normalize(typeName(type)) === normalize(query));
    const matches =
      exact === undefined
        ? [...catalog.keys()].filter((type) => terms.every((term) => catalog.get(type).includes(term)))
        : [exact];
    const shortcuts = [...new Set([...recent, ...DEFAULTS])].slice(0, 4);
    const expanded = !query && all.open;
    all.hidden = !!query;
    suggestions.hidden = expanded;
    suggestions.replaceChildren(...(query ? matches : shortcuts).map(button));
    models.replaceChildren();
    $('create-none').hidden = !query || matches.length > 0;
    if (expanded) {
      suggestions.replaceChildren();
      for (const [label, types] of GROUPS) {
        const section = document.createElement('section');
        const title = document.createElement('h3');
        title.textContent = label;
        section.append(title, ...types.map(button));
        models.append(section);
      }
    }
  }

  async function choose(type) {
    if (pending) {
      return;
    }
    pending = true;
    error.hidden = true;
    dialog.setAttribute('aria-busy', 'true');
    $('create-progress').hidden = false;
    dialog.querySelectorAll('[data-create], input').forEach((element) => {
      element.disabled = true;
    });
    try {
      let uid;
      if (type >= 1000) {
        const nfcv = [1500, 1501].includes(type);
        let length = 4;
        if (nfcv) {
          length = 8;
        } else if (TYPE2_PROFILES[type]) {
          length = 7;
        }
        const input = $('create-uid').value.trim();
        if (input) {
          uid = parseHex(input, length);
        } else if (!nfcv) {
          uid = Array.from(crypto.getRandomValues(new Uint8Array(length)));
          // A generated single-size UID must never start with the cascade marker 0x88.
          uid[0] = 0x04;
        }
      }
      if (await onCreate(type, uid)) {
        recent = [type, ...recent.filter((entry) => entry !== type)].slice(0, 4);
        try {
          localStorage.setItem(RECENT_MODELS, JSON.stringify(recent));
        } catch {
          /* Keep model history in memory if storage is unavailable. */
        }
      }
    } catch (failure) {
      showError(failure.message);
    } finally {
      pending = false;
      dialog.setAttribute('aria-busy', 'false');
      $('create-progress').hidden = true;
      dialog.querySelectorAll('[data-create], input').forEach((element) => {
        element.disabled = false;
      });
    }
  }

  dialog.onclick = (event) => {
    const selected = event.target.closest('[data-create]');
    if (selected) {
      choose(Number(selected.dataset.create));
    }
  };
  search.oninput = render;
  search.onkeydown = (event) => {
    const choices = suggestions.querySelectorAll('[data-create]');
    if (event.key === 'Enter' && search.value.trim() && choices.length === 1) {
      event.preventDefault();
      choices[0].click();
    }
  };
  all.ontoggle = render;
  dialog.oncancel = () => onCancel();
  $('create-cancel').onclick = () => {
    onCancel();
    close();
  };

  return {
    close,
    showError,
    open() {
      if (pending) {
        return;
      }
      error.hidden = true;
      search.value = '';
      all.open = false;
      $('create-uid').value = '';
      $('create-identity').open = false;
      render();
      dialog.showModal();
    }
  };
}
