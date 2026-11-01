import { offlineState, prepareOffline } from '../runtime/offline.mjs';
import { $ } from './device-view.mjs';

export function offlineView(isBusy) {
  let entries;
  let controller;
  let refreshPromise;
  let preparation;
  let message = '';

  const section = $('offline-resources');
  const status = $('offline-state');
  const prepare = $('offline-prepare');
  const stop = $('offline-stop');
  const progress = $('offline-progress');
  const notice = $('offline-message');

  function render() {
    const checking = !!refreshPromise;
    const busy = !!controller;
    const ready = entries?.every((entry) => entry.stored);
    const storageUnavailable = entries?.some((entry) => !entry.available);
    const saved = entries?.some((entry) => entry.stored);
    const session = entries?.some((entry) => entry.session);
    const needsPreparation = !checking && !busy && !!entries && !ready && !storageUnavailable;

    section.setAttribute('aria-busy', String(checking || busy));
    prepare.hidden = !needsPreparation;
    stop.hidden = !busy;
    prepare.disabled = isBusy();

    let detail = 'Resources are saved automatically in the background when Internet is available.';
    status.dataset.state = 'missing';
    status.textContent = 'Not prepared';
    if (checking) {
      status.textContent = 'Checking…';
    } else if (busy) {
      status.textContent = 'Preparing…';
    } else if (ready) {
      status.dataset.state = 'ready';
      status.textContent = 'Ready offline';
      detail = '';
    } else if (storageUnavailable) {
      status.dataset.state = 'unavailable';
      status.textContent = 'Storage unavailable';
      detail = 'Browser storage is unavailable. Resources cannot be kept for offline use.';
    } else if (saved) {
      status.textContent = 'Partially ready';
      detail = 'Some resources are missing. Download them before using the app without Internet.';
    } else if (session) {
      status.textContent = 'Session only';
      detail = 'Some resources are available only for this session. Save them to use them after a reload.';
    }

    notice.textContent = message || detail;
    notice.hidden = !notice.textContent;
  }

  function refresh() {
    if (controller) {
      return Promise.resolve();
    }
    if (!refreshPromise) {
      message = '';
      refreshPromise = offlineState()
        .then((next) => {
          entries = next;
        })
        .catch((error) => {
          message = error.message;
        })
        .finally(() => {
          refreshPromise = null;
          render();
        });
      render();
    }
    return refreshPromise;
  }

  async function start() {
    await preparation;
    if (isBusy()) {
      return;
    }
    await refresh();
    if (!entries || entries.some((entry) => !entry.available) || entries.every((entry) => entry.stored)) {
      return;
    }
    if (!controller) {
      preparation = prepareResources();
      await preparation;
    }
  }

  async function prepareResources() {
    if (controller || refreshPromise || isBusy()) {
      return;
    }

    controller = new AbortController();
    // WebKit can tear down cross-origin fetches before pagehide runs.
    window.addEventListener('beforeunload', pause);
    message = '';
    progress.hidden = false;
    $('offline-bytes').textContent = '';
    $('offline-progress-label').textContent = 'Checking saved resources…';
    $('offline-progress-bar').removeAttribute('value');
    render();

    try {
      entries = await prepareOffline(controller.signal, ({ loaded, total, phase }) => {
        const phases = { download: 'Downloading', verify: 'Verifying', save: 'Saving' };
        $('offline-progress-label').textContent = phases[phase];
        $('offline-bytes').textContent = `${(loaded / 1e6).toFixed(1)} / ${(total / 1e6).toFixed(1)} MB`;
        $('offline-progress-bar').max = total;
        $('offline-progress-bar').value = loaded;
      });
    } catch (error) {
      message = controller.signal.aborted ? 'Stopped. Resources already saved are kept.' : error.message;
      entries = await offlineState();
    } finally {
      window.removeEventListener('beforeunload', pause);
      controller = null;
      progress.hidden = true;
      render();
    }
  }

  prepare.onclick = () => {
    preparation = prepareResources();
  };

  function pause() {
    controller?.abort();
    render();
  }

  stop.onclick = () => {
    controller?.abort();
  };

  return { refresh, start, pause };
}
