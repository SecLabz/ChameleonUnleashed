import config from '../../config.json' with { type: 'json' };
import { inspectFirmware, MAXIMUM_SIZE } from './nordic-package.mjs';
import { loadCatalogue, downloadFirmware } from './firmware-catalogue.mjs';
import { connectDfu } from '../device/dfu-bluetooth.mjs';
import { ask } from './dialogs.mjs';

export async function bleFirmwareUpdate({ transport, signal, beforeDfu = () => {} } = {}) {
  const base = document.querySelector('meta[name="chameleon-resources"]')?.content ?? config.baseUrl;
  const dialog = document.createElement('dialog');
  dialog.className = 'app-dialog firmware-dialog';
  dialog.setAttribute('aria-labelledby', 'ble-update-title');
  dialog.innerHTML = `
    <h2 id="ble-update-title">Firmware update</h2>
    <p>Update over Bluetooth. Keep your Ultra nearby and this page open.</p>
    <div class="firmware-fields">
      <label class="field">Channel<select id="ble-update-channel" aria-label="Channel"><option value="stable">Stable</option><option value="development">Development</option></select></label>
      <label class="field">Firmware<select id="ble-update-source" aria-label="Firmware"></select></label>
      <label class="field">Version<select id="ble-update-version" aria-label="Version"></select></label>
    </div>
    <details><summary>Update from a file</summary><input id="ble-update-file" type="file" accept=".zip,application/zip" aria-label="Nordic firmware ZIP"></details>
    <p id="ble-update-status" role="status">Loading releases…</p>
    <progress id="ble-update-progress" max="1" value="0" aria-label="Firmware update progress" hidden></progress>
    <div class="dialog-actions"><button class="secondary" id="ble-update-close">Close</button><button class="primary-action" id="ble-update-install" disabled>Prepare update</button></div>`;
  const $ = (id) => dialog.querySelector('#ble-update-' + id);
  const controller = new AbortController();
  const abort = () => controller.abort(signal.reason);
  signal?.addEventListener('abort', abort, { once: true });
  const stopped = controller.signal;
  let catalogue;
  let generation = 0;
  let busy = false;
  let info;
  let images;
  let index = 0;
  let done = false;
  const sources = () => catalogue?.sources ?? [];
  const releases = () => sources().find((source) => source.id === $('source').value)?.releases ?? [];
  const selected = () => releases()[Number($('version').value)];
  const progress = (value, label) => {
    $('progress').hidden = false;
    $('progress').value = value;
    $('status').textContent = label;
  };
  const paint = () => {
    for (const control of dialog.querySelectorAll('select,input')) {
      control.disabled = busy || !!images || done;
    }
    $('install').disabled =
      busy || done || stopped.aborted || (!images && !$('file').files.length && !selected());
    $('install').textContent = images ? 'Connect DFU and install' : 'Prepare update';
    $('close').textContent = busy ? 'Stop' : 'Close';
  };
  const versions = () => {
    $('version').replaceChildren(...releases().map((release, i) => new Option(release.version, String(i))));
    $('status').textContent = releases().length
      ? 'Choose a release or a local Nordic DFU ZIP.'
      : 'No releases available. You can choose a local Nordic DFU ZIP.';
    paint();
  };
  const catalogueChanged = async () => {
    const current = ++generation;
    catalogue = null;
    $('source').replaceChildren();
    $('version').replaceChildren();
    paint();
    try {
      const result = await loadCatalogue(stopped, $('channel').value === 'development', base);
      if (current !== generation || stopped.aborted || busy || images || !dialog.open) {
        return;
      }
      catalogue = result;
      $('source').replaceChildren(...sources().map((source) => new Option(source.label, source.id)));
      versions();
    } catch (error) {
      if (current === generation && !stopped.aborted) {
        $('status').textContent = error.message;
        paint();
      }
    }
  };

  async function prepare() {
    const file = $('file').files[0];
    if (file && file.size > MAXIMUM_SIZE) {
      throw new Error('Firmware ZIP is too large.');
    }
    const release = selected();
    const development = $('channel').value === 'development';
    const bytes = file
      ? new Uint8Array(await file.arrayBuffer())
      : await downloadFirmware(
          release.packages[release.layout === 2 ? 'application' : 'full'],
          stopped,
          progress,
          development,
          base
        );
    info = await inspectFirmware(bytes, stopped);
    if (transport && !transport.closed) {
      const device = await transport.request('/api/device');
      if (device.busy) {
        throw new Error('Finish the active device operation before updating firmware.');
      }
    }
    const state = transport && !transport.closed ? await transport.request('/api/update') : null;
    if (!info.restore && state && state.layout !== 2) {
      throw new Error('Install the complete Unleashed CLI kit first to enable its web recovery bootloader.');
    }
    let warning =
      'Bluetooth DFU rewrites the application and web recovery. If interrupted, reconnect in DFU; USB DFU remains available.';
    if (!info.restore && !state) {
      warning =
        'This application ZIP requires an already installed Unleashed web recovery bootloader. If interrupted, reconnect in DFU; USB DFU remains available.';
    }
    if (info.restore) {
      warning =
        'Export saved tags first. Replacing system firmware may erase them; an interrupted installation may require USB DFU.';
    }
    let message = 'Select your Ultra in DFU to install.';
    if (transport && !transport.closed) {
      message =
        'Your Ultra will restart in DFU. Then select the same Ultra in the Bluetooth window to install.';
    }
    if (info.restore) {
      message = 'This installs the selected firmware and removes web recovery.';
    }
    const confirmed = await ask({
      title: 'Install firmware?',
      message,
      warning,
      confirmLabel: 'Continue',
      signal: stopped
    });
    if (!confirmed) {
      $('status').textContent = 'Update cancelled. Nothing installed.';
      $('progress').hidden = true;
      return;
    }
    if (transport && !transport.closed) {
      beforeDfu();
      progress(0, 'Restarting in DFU…');
      await transport.enterDfu(stopped);
    }
    images = [info.system, info.application].filter(Boolean);
    $('status').textContent =
      'Select your Ultra in DFU to install. If it is off, hold B while connecting USB.';
    $('progress').hidden = true;
  }

  async function install() {
    // Called synchronously from the button: requestDevice needs user activation.
    const connection = connectDfu({ signal: stopped });
    let dfu;
    try {
      dfu = await connection;
      const total = images.reduce((sum, image) => sum + image.payload.length, 0);
      const previous = images.slice(0, index).reduce((sum, image) => sum + image.payload.length, 0);
      const image = images[index];
      await dfu.flash(image, (value, label) =>
        progress((previous + value * image.payload.length) / total, label)
      );
      index++;
      if (index === images.length) {
        done = true;
        progress(
          1,
          info.restore
            ? 'Firmware accepted. Reconnect using its application.'
            : 'Firmware accepted. Reconnect your Ultra to verify the installed version.'
        );
      } else {
        $('status').textContent =
          'System firmware accepted. Select the same Ultra in DFU again to install the application.';
      }
    } finally {
      dfu?.close();
    }
  }

  $('install').onclick = async () => {
    if (busy || done || stopped.aborted) {
      return;
    }
    busy = true;
    paint();
    try {
      if (images) {
        await install();
      } else {
        await prepare();
      }
    } catch (error) {
      if (error.name !== 'NotFoundError') {
        $('status').textContent = stopped.aborted
          ? 'Stopped. Reconnect in DFU and install again.'
          : error.message;
      }
    } finally {
      busy = false;
      paint();
    }
  };
  $('channel').onchange = catalogueChanged;
  $('source').onchange = versions;
  $('version').onchange = paint;
  $('file').onchange = () => {
    $('status').textContent = $('file').files[0]?.name ?? 'Choose a release or a local Nordic DFU ZIP.';
    paint();
  };
  const close = () => {
    if (busy) {
      controller.abort(new Error('Update stopped'));
    } else {
      dialog.close();
    }
  };
  $('close').onclick = close;
  dialog.oncancel = (event) => {
    event.preventDefault();
    close();
  };
  document.body.append(dialog);
  dialog.showModal();
  void catalogueChanged();
  try {
    await new Promise((resolve) => dialog.addEventListener('close', resolve, { once: true }));
  } finally {
    controller.abort();
    signal?.removeEventListener('abort', abort);
    dialog.remove();
  }
  return done && !info.restore;
}
