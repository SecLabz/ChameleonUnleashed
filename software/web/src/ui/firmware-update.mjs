import { RESOURCE_BASE } from './update-config.mjs';
import { sha256 } from './update-sha256.mjs';
import {
  inspectFirmware,
  beginRequest,
  MAXIMUM_SIZE,
  MINIMUM_ZIP_SIZE,
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
const CATALOGUE_FORMAT = 1;
const RELEASES_PER_SOURCE = 5;
const SOURCE_IDS = ['unleashed', 'official'];
export function versionLabel(value) {
  return typeof value === 'string' ? value : `${value >>> 24}.${(value >>> 16) & 255}.${value & 65535}`;
}

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

const updateBase = () => new URL('updates/', new URL(RESOURCE_BASE, globalThis.location?.href));
function validAsset(asset) {
  return (
    asset &&
    Number.isInteger(asset.size) &&
    asset.size >= MINIMUM_ZIP_SIZE &&
    asset.size <= MAXIMUM_SIZE &&
    /^[a-f0-9]{64}$/.test(asset.sha256) &&
    new RegExp(`^(?:files/)?${asset.sha256}\\.zip$`).test(asset.path)
  );
}

export async function loadCatalogue(signal) {
  const response = await fetch(new URL('catalog.json', updateBase()), { signal, cache: 'no-store' });
  if (!response.ok) {
    throw new Error('Release list unavailable. You can still choose a local Nordic ZIP.');
  }
  const catalogue = await response.json();
  if (
    !catalogue ||
    catalogue.format !== CATALOGUE_FORMAT ||
    !Array.isArray(catalogue.sources) ||
    catalogue.sources.length > SOURCE_IDS.length ||
    new Set(catalogue.sources.map((source) => source?.id)).size !== catalogue.sources.length ||
    catalogue.sources.some(
      (source) =>
        !source ||
        !SOURCE_IDS.includes(source.id) ||
        typeof source.label !== 'string' ||
        !Array.isArray(source.releases) ||
        source.releases.length > RELEASES_PER_SOURCE ||
        new Set(source.releases.map((release) => release?.version)).size !== source.releases.length ||
        source.releases.some(
          (release) =>
            !release ||
            typeof release.version !== 'string' ||
            !release.version.length ||
            ![0, RECOVERY_LAYOUT].includes(release.layout) ||
            !validAsset(release.packages?.[release.layout === RECOVERY_LAYOUT ? 'application' : 'full'])
        )
    )
  ) {
    throw new Error('Invalid release catalogue.');
  }
  return catalogue;
}
export async function downloadFirmware(asset, signal, onProgress = () => {}) {
  if (!validAsset(asset)) {
    throw new Error('Invalid firmware download metadata.');
  }
  const file = await fetch(new URL(asset.path, updateBase()), { signal, cache: 'no-store' });
  if (!file.ok || !file.body) {
    throw new Error('Firmware download failed. Choose a local Nordic ZIP to continue.');
  }
  const bytes = new Uint8Array(asset.size);
  const reader = file.body.getReader();
  let offset = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      if (value.length > bytes.length - offset) {
        throw new Error('The firmware download is larger than expected.');
      }
      bytes.set(value, offset);
      offset += value.length;
      onProgress(offset / bytes.length, 'Downloading firmware…');
    }
  } finally {
    await reader.cancel().catch(() => {});
  }
  if (offset !== bytes.length || (await sha256(bytes, signal)) !== asset.sha256) {
    throw new Error('Firmware download verification failed. Try again.');
  }
  return bytes;
}

if (typeof document !== 'undefined') {
  const $ = (id) => document.getElementById(id);
  let controller;
  let catalogue;
  let device;
  let localFile;
  let localInfo;
  let fileSelection = 0;
  let busy = false;
  let restored = false;
  let canReturn = false;
  const showProgress = (value, label) => {
    $('progress').hidden = false;
    $('progress').value = value;
    $('message').textContent = label;
    if (label === 'Verifying and activating firmware…') {
      $('stop').hidden = true;
    }
  };
  const selectedSource = () => catalogue?.sources.find((source) => source.id === $('source').value);
  const selectedRelease = () =>
    selectedSource()?.releases.find((release) => release.version === $('version').value);
  const updateControls = () => {
    const locked = busy || restored;
    for (const id of ['source', 'version', 'file', 'wipe', 'restart']) {
      $(id).disabled = locked;
    }
    $('file').disabled ||= Boolean(device && device.layout !== RECOVERY_LAYOUT);
    $('update').disabled = locked || device?.layout !== 2 || !(localFile ? localInfo : selectedRelease());
    $('restart').hidden = locked || !canReturn;
    $('exit-hint').hidden = $('restart').hidden || device?.mode !== 'recovery';
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
    $('release').textContent = release ? '' : 'No compatible release available';
    $('update').textContent =
      !release || release.layout === RECOVERY_LAYOUT ? 'Install update' : 'Install complete firmware';
    $('transition').hidden = !release || release.layout === RECOVERY_LAYOUT;
  };
  const selectSource = () => {
    $('version').replaceChildren();
    for (const release of selectedSource()?.releases ?? []) {
      $('version').add(
        new Option(`${release.version}${release.prerelease ? ' (preview)' : ''}`, release.version)
      );
    }
    const stable = selectedSource()?.releases.find((release) => !release.prerelease);
    if (stable) {
      $('version').value = stable.version;
    }
    selectRelease();
  };
  const refresh = async (showConnectionError = true) => {
    try {
      device = await (await request('/api/update')).json();
      $('device').dataset.state = device.mode === 'recovery' ? 'recovery' : 'connected';
      $('device').textContent =
        device.mode === 'recovery'
          ? 'Ultra connected · Recovery mode'
          : `Ultra connected · v${versionLabel(device.version)}`;
      canReturn = device.mode === 'application' || Boolean(device.valid);
      if (device.layout !== RECOVERY_LAYOUT) {
        $('message').textContent = 'Install the Nordic ZIP recovery bootloader through classic DFU first.';
      }
      selectRelease();
    } catch {
      device = undefined;
      canReturn = false;
      $('device').textContent = 'Ultra not connected';
      $('device').dataset.state = 'disconnected';
      updateControls();
      if (showConnectionError) {
        $('message').textContent = 'Connect your Ultra, then reload this page.';
      }
    }
  };
  const fetchCatalogue = async () => {
    try {
      const next = await loadCatalogue();
      if (busy || restored) {
        return;
      }
      catalogue = next;
      $('source').replaceChildren();
      for (const source of catalogue.sources) {
        $('source').add(new Option(source.label, source.id));
      }
      selectSource();
    } catch (error) {
      if (!busy && !restored) {
        $('release').textContent = error.message;
      }
    }
  };
  const run = async (file) => {
    if (busy || restored) {
      return;
    }
    busy = true;
    controller = new AbortController();
    updateControls();
    $('progress').hidden = true;
    $('title').textContent = '';
    $('message').textContent = 'Checking firmware…';
    $('stop').hidden = false;
    let replacement = false;
    let recoveryUpdate = false;
    let finished = false;
    try {
      if (file && file.size > MAXIMUM_SIZE) {
        throw new Error('Choose a Nordic firmware ZIP smaller than 4 MiB.');
      }
      const release = selectedRelease();
      const asset = release?.packages[release.layout === RECOVERY_LAYOUT ? 'application' : 'full'];
      const bytes = file
        ? new Uint8Array(await file.arrayBuffer())
        : await downloadFirmware(asset, controller.signal, showProgress);
      const info = await inspectFirmware(bytes, controller.signal);
      replacement = info.restore;
      // Another page or a classic DFU update may have changed recovery since
      // this page connected. Confirm against the current device, not that snapshot.
      const current = await (await request('/api/update', undefined, controller.signal)).json();
      recoveryUpdate = !replacement && info.recoveryCrc !== current.recovery_crc32;
      if (
        (replacement || recoveryUpdate) &&
        !confirm(
          replacement
            ? 'Install complete firmware? This removes web recovery. An interrupted installation may require classic DFU.' +
                ($('wipe').checked
                  ? ' All saved tags and settings will be erased.'
                  : ' Stored data may be incompatible with the selected firmware.')
            : 'This version also replaces web recovery. An interrupted recovery replacement may require classic DFU. Continue?'
        )
      ) {
        return;
      }
      const result = await installFirmware(bytes, {
        signal: controller.signal,
        onProgress: showProgress,
        allowRestore: replacement,
        allowRecoveryUpdate: recoveryUpdate,
        wipe: $('wipe').checked
      });
      restored = result.mode === 'restored';
      $('title').textContent =
        result.mode === 'restored' ? 'Firmware transferred and verified' : 'Your Ultra is up to date';
      $('message').textContent =
        result.mode === 'restored'
          ? 'Your Ultra is restarting with the selected firmware. This web interface is removed; reinstall Unleashed through classic DFU to return.'
          : 'Firmware verified and running. Your slots and settings are preserved.';
      if (result.mode !== 'restored') {
        device = result;
        $('device').textContent = `Ultra connected · v${versionLabel(result.version)}`;
        canReturn = true;
      }
      finished = true;
    } catch (error) {
      let message = error.message;
      if (controller.signal.aborted) {
        message =
          replacement || recoveryUpdate
            ? 'Installation stopped. If this page no longer opens, finish installation through classic DFU.'
            : 'Update stopped. Reopen this page to install again.';
      }
      $('message').textContent = message;
    } finally {
      $('stop').hidden = true;
      if (!finished) {
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
    $('message').textContent = '';
    $('progress').hidden = true;
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
          throw new Error('Choose a Nordic firmware ZIP smaller than 4 MiB.');
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
      $('message').textContent = error.message;
    }
    selectRelease();
  };
  $('stop').onclick = () => controller?.abort(new Error('Installation cancelled'));
  $('restart').onclick = async () => {
    if (busy || restored) {
      return;
    }
    busy = true;
    updateControls();
    try {
      if (device?.mode === 'recovery') {
        $('message').textContent = 'Starting the installed firmware…';
        device = await startApplication();
      }
      location.href = '/';
    } catch (error) {
      $('message').textContent = error.message;
    } finally {
      busy = false;
      selectRelease();
    }
  };
  refresh();
  fetchCatalogue();
}
