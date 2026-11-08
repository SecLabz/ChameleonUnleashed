import config from '../../config.json' with { type: 'json' };
import { connectDfu } from '../device/dfu-bluetooth.mjs';
import { ask } from './dialogs.mjs';
import { updateForm, versionLabel } from './firmware-update.mjs';
import { RECOVERY_LAYOUT } from './nordic-package.mjs';

// Mirrors public/update.html: both transports present the same form.
const SHEET = `
  <div class="update-page">
    <div class="update-heading">
      <h2 id="fw-heading">Firmware update<span aria-hidden="true">.</span></h2>
      <p class="update-device" id="fw-device" role="status" data-state="connecting">Connecting to your Ultra…</p>
    </div>
    <section class="update-card">
      <div class="update-fields">
        <div class="update-channel">
          <label for="fw-channel">Channel</label>
          <select id="fw-channel" aria-label="Update channel">
            <option value="stable">Stable</option>
            <option value="development">Development</option>
          </select>
        </div>
        <div>
          <label for="fw-source">Firmware</label>
          <select id="fw-source" aria-label="Firmware source"></select>
        </div>
        <div>
          <label for="fw-version">Version</label>
          <select id="fw-version" aria-label="Firmware version"></select>
        </div>
      </div>
      <p class="update-note" id="fw-transition" hidden>
        Complete firmware removes web recovery and may erase saved tags. Export them first.
      </p>
      <details>
        <summary>Update from a file</summary>
        <label class="update-drop">
          <input id="fw-file" type="file" accept=".zip,application/zip" aria-label="Nordic firmware ZIP">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"
            stroke-linejoin="round" aria-hidden="true">
            <path d="M12 15V4M8 8l4-4 4 4M5 15v3a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-3" />
          </svg>
          Drop or choose a Nordic DFU ZIP
        </label>
      </details>
      <p class="update-hint" id="fw-release" role="status">Loading releases…</p>
      <div class="update-status">
        <h3 id="fw-title"></h3>
        <p id="fw-message" role="status"></p>
        <progress id="fw-progress" max="1" value="0" aria-label="Firmware update progress" hidden></progress>
      </div>
      <button class="update-primary" id="fw-update" type="button" disabled>Install update</button>
      <button class="update-primary" id="fw-next" type="button" hidden></button>
      <button class="update-stop" id="fw-stop" type="button" hidden>Stop</button>
    </section>
    <footer class="update-footer">
      <button class="update-return" id="fw-restart" type="button" hidden>Return to Chameleon</button>
    </footer>
  </div>`;

/**
 * The Bluetooth counterpart of the USB update page. A browser opens a Bluetooth
 * chooser only from a click, so the sheet asks for one per DFU image and one to
 * reconnect and verify the installed version.
 * Resolves true when an application was transferred but not verified.
 */
export async function bleFirmwareUpdate({ transport, signal, beforeDfu = () => {}, reconnect } = {}) {
  signal?.throwIfAborted();
  const lifetime = new AbortController();
  const dialog = document.createElement('dialog');
  dialog.className = 'update-sheet';
  dialog.setAttribute('aria-labelledby', 'fw-heading');
  dialog.innerHTML = SHEET;
  const $ = (id) => dialog.querySelector('#fw-' + id);
  const live = () => transport && !transport.closed;
  const pill = (state, label) => {
    $('device').dataset.state = state;
    $('device').textContent = label;
  };
  const connected = (version) => ({
    state: 'connected',
    label: version === undefined ? 'Ultra connected' : `Ultra connected · v${versionLabel(version)}`,
    ready: true,
    exit: true
  });
  let dfu = false;
  let unverified = false;

  // The main button becomes the click a chooser needs. An installation can be
  // stopped while it waits; a cancelled chooser or failed connection re-arms it.
  const step = (label, message, action, stopped) =>
    new Promise((resolve, reject) => {
      const next = $('next');
      let settled = false;
      const abort = () => settle(reject, stopped.reason);
      const settle = (finish, value) => {
        if (settled) {
          return;
        }
        settled = true;
        stopped?.removeEventListener('abort', abort);
        next.hidden = true;
        $('update').hidden = false;
        finish(value);
      };
      const arm = (text) => {
        $('update').hidden = true;
        $('progress').hidden = true;
        $('stop').hidden = !stopped;
        $('message').textContent = text;
        next.textContent = label;
        next.hidden = false;
        next.disabled = false;
        next.onclick = () => {
          next.disabled = true;
          // Called synchronously: the chooser still needs this click's activation.
          action().then(
            (value) => settle(resolve, value),
            (error) => {
              if (stopped?.aborted) {
                settle(reject, error);
              } else {
                arm(error.name === 'NotFoundError' ? message : error.message);
              }
            }
          );
        };
      };
      if (stopped?.aborted) {
        abort();
        return;
      }
      stopped?.addEventListener('abort', abort, { once: true });
      arm(message);
    });

  const verify = (expected) =>
    step(
      'Reconnect and verify',
      'Firmware transferred. When your Ultra has restarted, reconnect it to verify the installed version.',
      async () => {
        const next = await reconnect(lifetime.signal);
        lifetime.signal.throwIfAborted();
        const running = await next.request('/api/update');
        if (running.version !== expected) {
          throw new Error(
            'The Ultra returned with an unexpected firmware version. Reconnect to check again.'
          );
        }
        return next;
      },
      lifetime.signal
    ).then((next) => {
      lifetime.signal.throwIfAborted();
      transport = next;
      unverified = false;
      $('title').textContent = 'Your Ultra is up to date';
      $('message').textContent = 'Firmware verified and running. Your slots and settings are preserved.';
      pill('connected', `Ultra connected · v${versionLabel(expected)}`);
    });

  const stop = updateForm($, {
    base: document.querySelector('meta[name="chameleon-resources"]')?.content ?? config.baseUrl,
    async refresh() {
      if (!live()) {
        return {
          state: 'disconnected',
          label: 'Ultra not connected',
          ready: true,
          exit: true,
          note: 'You will select your Ultra in DFU during installation.'
        };
      }
      const state = await transport.request('/api/update').catch(() => null);
      return connected(state?.version);
    },
    async install(bytes, info, { signal, progress }) {
      dfu = false;
      const restarting = live();
      if (restarting) {
        if ((await transport.request('/api/device')).busy) {
          throw new Error('Finish the active device operation before updating firmware.');
        }
        const state = await transport.request('/api/update');
        if (!info.restore && state.layout !== RECOVERY_LAYOUT) {
          throw new Error(
            'Install the complete Unleashed CLI kit first to enable its web recovery bootloader.'
          );
        }
      }
      const confirmed = await ask({
        title: info.restore ? 'Install complete firmware?' : 'Install firmware?',
        message: restarting
          ? 'Your Ultra restarts in DFU, then you select it in the Bluetooth window.'
          : 'You select your Ultra in DFU in the Bluetooth window.',
        warning: info.restore
          ? 'This removes web recovery. Export saved tags first: stored data may be erased or incompatible. An interrupted installation may require classic DFU.'
          : 'Bluetooth installation also replaces web recovery. An interrupted installation may require classic DFU.',
        confirmLabel: 'Install',
        signal
      });
      if (!confirmed) {
        return;
      }
      if (restarting) {
        beforeDfu();
        progress(0, 'Restarting in DFU…');
        await transport.enterDfu(signal);
        pill('recovery', 'Ultra in DFU mode');
      }
      dfu = true;
      const images = [info.system, info.application].filter(Boolean);
      const total = images.reduce((sum, image) => sum + image.payload.length, 0);
      let installed = 0;
      for (const image of images) {
        let message = 'Select your Ultra in DFU. If it is off, hold B while connecting USB.';
        if (installed) {
          message = 'System firmware installed. Select your Ultra in DFU again to install the application.';
        } else if (restarting) {
          message = 'Your Ultra restarted in DFU. Select it in the Bluetooth window.';
        }
        const session = await step('Select Ultra in DFU', message, () => connectDfu({ signal }), signal);
        try {
          await session.flash(image, (value, label) =>
            progress((installed + value * image.payload.length) / total, label)
          );
        } finally {
          session.close();
        }
        installed += image.payload.length;
      }
      const restarted = { state: 'connecting', label: 'Ultra restarting', ready: true, exit: true };
      if (info.restore) {
        return {
          title: 'Firmware transferred and verified',
          message:
            'Your Ultra is restarting with the selected firmware. Web recovery is removed; reinstall Unleashed through classic DFU to return.',
          device: restarted,
          final: true
        };
      }
      unverified = true;
      void verify(info.version).catch(() => {}); // Closing the sheet cancels optional verification.
      return { title: '', message: $('message').textContent, device: restarted, final: true };
    },
    stopped: () =>
      dfu
        ? 'Update stopped. Install again and select your Ultra in DFU; classic DFU remains available.'
        : 'Update stopped. Nothing installed.',
    exit: () => dialog.close()
  });

  dialog.oncancel = (event) => {
    event.preventDefault();
    if (!$('restart').hidden) {
      dialog.close();
    }
  };
  const close = () => dialog.close();
  signal?.addEventListener('abort', close, { once: true });
  document.body.append(dialog);
  dialog.showModal();
  try {
    await new Promise((resolve) => dialog.addEventListener('close', resolve, { once: true }));
  } finally {
    lifetime.abort(new Error('Firmware update closed'));
    stop();
    signal?.removeEventListener('abort', close);
    dialog.remove();
  }
  return unverified;
}
