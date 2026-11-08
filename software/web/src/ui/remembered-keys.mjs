import { classicKeyCache } from '../recovery/classic-key-cache.mjs';
import { ask } from './dialogs.mjs';

export function rememberedKeysView() {
  const button = document.getElementById('remembered-keys-clear');
  const status = document.getElementById('remembered-keys-status');
  let busy = false;
  let clearing = false;
  let available = false;
  let count = 0;

  function paint() {
    button.disabled = busy || clearing || !available || !count;
  }

  function refresh() {
    ({ available, count } = classicKeyCache.state());
    status.textContent = available
      ? `${count ? `${count} ${count === 1 ? 'tag' : 'tags'}` : 'No tags yet'} · Saved in this browser`
      : 'Browser storage unavailable';
    paint();
  }

  button.onclick = async () => {
    if (button.disabled) {
      return;
    }
    clearing = true;
    paint();
    try {
      if (
        await ask({
          title: 'Clear remembered tag keys?',
          message:
            'Future reads will search for these keys again. Your personal dictionary and documents will be kept.',
          confirmLabel: 'Clear keys'
        })
      ) {
        if (!busy && !classicKeyCache.clear()) {
          status.textContent = 'Could not clear remembered keys. Try again.';
          return;
        }
      }
      refresh();
    } finally {
      clearing = false;
      paint();
      if (!button.disabled) {
        button.focus();
      } else {
        status.focus();
      }
    }
  };

  window.addEventListener('storage', refresh);
  refresh();
  return {
    refresh,
    setBusy(value) {
      busy = value;
      paint();
    }
  };
}
