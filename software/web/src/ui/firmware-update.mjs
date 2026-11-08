import { RESOURCE_BASE } from './update-config.mjs';
import {
  loadCatalogue as readCatalogue,
  downloadFirmware as download,
  versionLabel
} from './firmware-catalogue.mjs';
export { versionLabel };
import {
  inspectFirmware,
  beginRequest,
  MAXIMUM_SIZE,
  UPDATE_CHUNK_BYTES,
  RECOVERY_LAYOUT
} from './nordic-package.mjs';
export { inspectFirmware, MAXIMUM_SIZE };
const REQUEST_TIMEOUT_MS = 10000;
const RECONNECT_TIMEOUT_MS = 45000;
const RECONNECT_REQUEST_TIMEOUT_MS = 1000;
const RECONNECT_POLL_MS = 500;
const RETRY_DELAY_MS = 500;
const MAX_RETRIES = 2;

async function request(path, body, signal, timeout = REQUEST_TIMEOUT_MS) {
  signal?.throwIfAborted();
  const controller = new AbortController();
  const stop = () => controller.abort(signal.reason);
  signal?.addEventListener('abort', stop, { once: true });
  const timer = setTimeout(() => controller.abort(new TypeError('USB request timed out')), timeout);
  let response;
  try {
    response = await fetch(path, {
      method: body === undefined ? 'GET' : 'POST',
      headers: body === undefined ? {} : { 'Content-Type': 'application/octet-stream' },
      body,
      signal: controller.signal,
      cache: 'no-store'
    });
    const data = await response.arrayBuffer();
    response = new Response(data.byteLength ? data : null, {
      status: response.status,
      headers: response.headers
    });
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', stop);
  }
  if (!response.ok) {
    const errors = {
      422: 'Firmware verification failed. Download a matching firmware and try again.',
      409: 'The Ultra is busy or the update session changed. Reopen this page and try again.'
    };
    throw new Error(
      errors[response.status] || `The Ultra could not complete this step (${response.status}).`
    );
  }
  return response;
}

const delay = (time, signal) =>
  new Promise((resolve, reject) => {
    signal?.throwIfAborted();
    const stop = () => {
      clearTimeout(timer);
      reject(signal.reason);
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', stop);
      resolve();
    }, time);
    signal?.addEventListener('abort', stop, { once: true });
  });

async function retry(operation, signal) {
  for (let attempt = 0; ; attempt++) {
    try {
      return await operation();
    } catch (error) {
      signal?.throwIfAborted();
      if (!(error instanceof TypeError) || attempt >= MAX_RETRIES) {
        throw error;
      }
      await delay(RETRY_DELAY_MS, signal);
    }
  }
}

async function waitFor(mode, signal) {
  const deadline = Date.now() + RECONNECT_TIMEOUT_MS;
  while (Date.now() < deadline) {
    signal?.throwIfAborted();
    try {
      const state = await (
        await request('/api/update', undefined, signal, RECONNECT_REQUEST_TIMEOUT_MS)
      ).json();
      if (state.mode === mode) {
        return state;
      }
    } catch {
      signal?.throwIfAborted();
    }
    await delay(RECONNECT_POLL_MS, signal);
  }
  throw new Error('The Ultra has not reconnected. Reconnect USB and reopen this page.');
}

export async function startApplication(signal) {
  await request('/api/update/reboot', new Uint8Array(), signal).catch((error) => {
    if (!(error instanceof TypeError)) {
      throw error;
    }
  });
  // The reboot response precedes the actual reset. Loading / immediately would
  // serve the recovery screen again and leave the browser displaying that page.
  return waitFor('application', signal);
}

export async function installFirmware(
  bytes,
  { signal, onProgress = () => {}, allowRestore = false, allowRecoveryUpdate = false, wipe = false } = {}
) {
  const info = await inspectFirmware(bytes, signal);
  let state = await (await request('/api/update', undefined, signal)).json();
  if (state.layout !== RECOVERY_LAYOUT) {
    throw new Error('Install the Nordic ZIP recovery bootloader through classic DFU first.');
  }
  if (info.restore && !allowRestore) {
    throw new Error('Complete firmware replacement must be confirmed first.');
  }
  if (!info.restore && info.recoveryCrc !== state.recovery_crc32 && !allowRecoveryUpdate) {
    throw new Error('This ZIP also replaces recovery. Confirm the complete recovery update first.');
  }
  if (state.mode === 'application') {
    onProgress(0, 'Opening recovery…');
    await request('/api/update/enter', new Uint8Array(), signal).catch((error) => {
      if (!(error instanceof TypeError)) {
        throw error;
      }
    });
    state = await waitFor('recovery', signal);
  }
  if (state.mode !== 'recovery') {
    throw new Error('Recovery mode is not available.');
  }
  await retry(() => request('/api/update/begin', beginRequest(info, wipe), signal), signal);
  state = await (await request('/api/update', undefined, signal)).json();
  const session = state.session;
  const images = info.restore
    ? [
        ['system', info.system.payload],
        ['write', info.application.payload]
      ]
    : [['write', info.application.payload]];
  const total = images.reduce((sum, image) => sum + image[1].length, 0);
  let transferred = 0;
  for (const [endpoint, payload] of images) {
    for (let offset = 0; offset < payload.length; offset += UPDATE_CHUNK_BYTES) {
      const chunk = payload.subarray(offset, Math.min(offset + UPDATE_CHUNK_BYTES, payload.length));
      await retry(
        () => request(`/api/update/${endpoint}?session=${session}&offset=${offset}`, chunk, signal),
        signal
      );
      onProgress((transferred + offset + chunk.length) / total, 'Installing firmware…');
    }
    transferred += payload.length;
  }
  onProgress(1, 'Verifying and activating firmware…');
  await retry(() => request(`/api/update/commit?session=${session}`, new Uint8Array(), signal), signal);
  onProgress(1, 'Restarting your Ultra…');
  await request('/api/update/reboot', new Uint8Array(), signal).catch((error) => {
    if (!(error instanceof TypeError)) {
      throw error;
    }
  });
  if (info.restore) {
    return { mode: 'restored', version: info.version };
  }
  const running = await waitFor('application', signal);
  if (running.version !== info.version) {
    throw new Error('The Ultra returned with an unexpected firmware version.');
  }
  return running;
}

export const loadCatalogue = (signal, development = false, base = RESOURCE_BASE) =>
  readCatalogue(signal, development, base);
export const downloadFirmware = (asset, signal, onProgress, development = false, base = RESOURCE_BASE) =>
  download(asset, signal, onProgress, development, base);

const TOO_LARGE = 'Choose a Nordic firmware ZIP smaller than 4 MiB.';

// Shared by the USB page and the Bluetooth sheet. The driver describes the
// Ultra, installs over its transport, explains a stop and leaves the form.
export function updateForm($, driver) {
  let controller;
  let catalogue;
  let device;
  let localFile;
  let localInfo;
  let fileSelection = 0;
  let catalogueRequest = 0;
  let busy = false;
  let final = false;
  const say = (text) => {
    $('message').textContent = text;
  };
  const showProgress = (value, label) => {
    $('progress').hidden = false;
    $('progress').value = value;
    say(label);
    if (label === 'Verifying and activating firmware…') {
      $('stop').hidden = true;
    }
  };
  const show = (next) => {
    device = next;
    $('device').dataset.state = next.state;
    $('device').textContent = next.label;
  };
  const development = () => $('channel').value === 'development';
  const selectedSource = () => catalogue?.sources.find((source) => source.id === $('source').value);
  const selectedRelease = () =>
    selectedSource()?.releases.find((release) => release.version === $('version').value);
  const updateControls = () => {
    const locked = busy || final;
    for (const id of ['channel', 'source', 'version', 'file', 'wipe']) {
      if ($(id)) {
        $(id).disabled = locked;
      }
    }
    $('file').disabled ||= Boolean(device?.blocked);
    $('update').disabled = locked || !device?.ready || !(localFile ? localInfo : selectedRelease());
    $('restart').hidden = busy || !device?.exit;
    if ($('exit-hint')) {
      $('exit-hint').hidden = $('restart').hidden || device.state !== 'recovery';
    }
  };
  const selectRelease = () => {
    updateControls();
    if (localFile) {
      $('release').textContent = localFile.name;
      $('update').textContent = 'Install selected ZIP';
      $('transition').hidden = !localInfo?.restore;
      return;
    }
    const release = selectedRelease();
    const complete = release && release.layout !== RECOVERY_LAYOUT;
    $('release').textContent = release ? '' : 'No compatible release available';
    $('update').textContent = complete ? 'Install complete firmware' : 'Install update';
    $('transition').hidden = !complete;
  };
  const selectSource = () => {
    $('version').replaceChildren();
    for (const release of selectedSource()?.releases ?? []) {
      const label = development() ? release.version.replace('-dev.', ' · ') : release.version;
      $('version').add(
        new Option(`${label}${release.prerelease && !development() ? ' (preview)' : ''}`, release.version)
      );
    }
    const stable = selectedSource()?.releases.find((release) => !release.prerelease);
    if (stable) {
      $('version').value = stable.version;
    }
    selectRelease();
  };
  const refresh = async (note = true) => {
    show(await driver.refresh());
    if (note && device.note) {
      say(device.note);
    }
    selectRelease();
  };
  const fetchCatalogue = async () => {
    const generation = ++catalogueRequest;
    catalogue = undefined;
    $('source').replaceChildren(new Option('Loading…', ''));
    $('version').replaceChildren();
    $('release').textContent = 'Loading releases…';
    updateControls();
    try {
      const next = await loadCatalogue(undefined, development(), driver.base);
      if (generation !== catalogueRequest || busy || final) {
        return;
      }
      catalogue = next;
      $('source').replaceChildren(...catalogue.sources.map((source) => new Option(source.label, source.id)));
      selectSource();
    } catch (error) {
      if (generation === catalogueRequest && !busy && !final) {
        $('release').textContent = error.message;
      }
    }
  };
  const run = async (file) => {
    if (busy || final) {
      return;
    }
    busy = true;
    controller = new AbortController();
    updateControls();
    $('progress').hidden = true;
    $('title').textContent = '';
    say('Checking firmware…');
    $('stop').hidden = false;
    let info;
    let result;
    try {
      if (file && file.size > MAXIMUM_SIZE) {
        throw new Error(TOO_LARGE);
      }
      const release = selectedRelease();
      const bytes = file
        ? new Uint8Array(await file.arrayBuffer())
        : await downloadFirmware(
            release?.packages[release.layout === RECOVERY_LAYOUT ? 'application' : 'full'],
            controller.signal,
            showProgress,
            development(),
            driver.base
          );
      info = await inspectFirmware(bytes, controller.signal);
      result = await driver.install(bytes, info, {
        signal: controller.signal,
        progress: showProgress,
        wipe: $('wipe')?.checked
      });
      if (result) {
        $('title').textContent = result.title;
        say(result.message);
        show(result.device);
        final = Boolean(result.final);
      } else {
        $('progress').hidden = true;
        say('Update cancelled. Nothing installed.');
      }
    } catch (error) {
      say(controller.signal.aborted ? driver.stopped(info) : error.message);
    } finally {
      $('stop').hidden = true;
      if (!result) {
        await refresh(false);
      }
      busy = false;
      selectRelease();
    }
  };
  const clearLocal = () => {
    fileSelection++;
    localFile = undefined;
    localInfo = undefined;
    $('file').value = '';
  };
  const resetStatus = () => {
    $('title').textContent = '';
    say('');
    $('progress').hidden = true;
  };
  $('channel').onchange = () => {
    resetStatus();
    clearLocal();
    fetchCatalogue();
  };
  $('source').onchange = () => {
    resetStatus();
    clearLocal();
    selectSource();
  };
  $('version').onchange = () => {
    resetStatus();
    clearLocal();
    selectRelease();
  };
  $('update').onclick = () => run(localFile);
  $('file').onchange = async () => {
    resetStatus();
    const selection = ++fileSelection;
    const file = $('file').files[0];
    localFile = file;
    localInfo = undefined;
    selectRelease();
    try {
      if (file) {
        if (file.size > MAXIMUM_SIZE) {
          throw new Error(TOO_LARGE);
        }
        const bytes = new Uint8Array(await file.arrayBuffer());
        if (selection !== fileSelection) {
          return;
        }
        const info = await inspectFirmware(bytes);
        if (selection !== fileSelection) {
          return;
        }
        localInfo = info;
      }
    } catch (error) {
      if (selection !== fileSelection) {
        return;
      }
      say(error.message);
    }
    selectRelease();
  };
  $('stop').onclick = () => controller?.abort(new Error('Installation cancelled'));
  $('restart').onclick = async () => {
    if (busy) {
      return;
    }
    busy = true;
    updateControls();
    try {
      await driver.exit(device, say);
    } catch (error) {
      say(error.message);
    } finally {
      busy = false;
      selectRelease();
    }
  };
  refresh();
  fetchCatalogue();
  return () => controller?.abort(new Error('Update stopped'));
}

// Only the USB update page has #update; the Bluetooth sheet mounts its own form.
if (typeof document !== 'undefined' && document.getElementById('update')) {
  let risky = false;
  const describe = (state) => {
    const recovery = state.mode === 'recovery';
    const blocked = state.layout !== RECOVERY_LAYOUT;
    return {
      state: recovery ? 'recovery' : 'connected',
      label: recovery
        ? 'Ultra connected · Recovery mode'
        : `Ultra connected · v${versionLabel(state.version)}`,
      ready: !blocked,
      blocked,
      exit: !recovery || Boolean(state.valid),
      note: blocked ? 'Install the Nordic ZIP recovery bootloader through classic DFU first.' : ''
    };
  };
  updateForm((id) => document.getElementById(id), {
    base: RESOURCE_BASE,
    async refresh() {
      try {
        return describe(await (await request('/api/update')).json());
      } catch {
        return {
          state: 'disconnected',
          label: 'Ultra not connected',
          note: 'Connect your Ultra, then reload this page.'
        };
      }
    },
    async install(bytes, info, { signal, progress, wipe }) {
      risky = false;
      // Another page or a classic DFU update may have changed recovery since
      // this page connected. Confirm against the current device, not that snapshot.
      const current = await (await request('/api/update', undefined, signal)).json();
      const recoveryUpdate = !info.restore && info.recoveryCrc !== current.recovery_crc32;
      if (
        (info.restore || recoveryUpdate) &&
        !confirm(
          info.restore
            ? 'Install complete firmware? This removes web recovery. An interrupted installation may require classic DFU.' +
                (wipe
                  ? ' All saved tags and settings will be erased.'
                  : ' Stored data may be incompatible with the selected firmware.')
            : 'This version also replaces web recovery. An interrupted recovery replacement may require classic DFU. Continue?'
        )
      ) {
        return;
      }
      risky = info.restore || recoveryUpdate;
      const result = await installFirmware(bytes, {
        signal,
        onProgress: progress,
        allowRestore: info.restore,
        allowRecoveryUpdate: recoveryUpdate,
        wipe
      });
      return result.mode === 'restored'
        ? {
            title: 'Firmware transferred and verified',
            message:
              'Your Ultra is restarting with the selected firmware. This web interface is removed; reinstall Unleashed through classic DFU to return.',
            device: { state: 'connecting', label: 'Ultra restarting' },
            final: true
          }
        : {
            title: 'Your Ultra is up to date',
            message: 'Firmware verified and running. Your slots and settings are preserved.',
            device: describe(result)
          };
    },
    stopped: (info) =>
      info && risky
        ? 'Installation stopped. If this page no longer opens, finish installation through classic DFU.'
        : 'Update stopped. Reopen this page to install again.',
    async exit(device, say) {
      if (device.state === 'recovery') {
        say('Starting the installed firmware…');
        await startApplication();
      }
      location.href = '/';
    }
  });
}
