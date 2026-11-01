import { request, hex } from '../device/transport.mjs';
import { decodeSlot } from '../device/slots.mjs';

export const $ = (id) => document.getElementById(id);

export const escape = (text) =>
  String(text).replace(
    /[&<>"']/g,
    (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]
  );

const ACTIONS = [
  'Nothing',
  'Next slot',
  'Previous slot',
  'Clone a tag UID',
  'Show battery level',
  'NFC field on/off'
];
const SETTINGS = ['animation', 'press_a', 'press_b', 'long_a', 'long_b', 'sleep', 'long_ms', 'pairing'];

export function deviceViews({ notify, onConnection }) {
  let info;
  let refreshPromise;

  function render() {
    const settings = info.settings;
    for (const button of document.querySelectorAll('[data-connection]')) {
      button.innerHTML = `<span class="cu-batt" aria-hidden="true"><i
        style="width:${info.battery_percent}%"></i></span>${info.battery_percent}%`;
      button.setAttribute('aria-label', `Battery ${info.battery_percent} percent. Open device details.`);
      button.dataset.state = info.battery_percent <= 15 ? 'low' : 'connected';
    }

    const places = ['hf', 'lf'].filter((sense) => info.slot[sense].type && info.slot[sense].enabled);
    const frequencies = places.map((sense) => (sense === 'hf' ? 'NFC' : 'RFID')).join(' + ');
    let slotLabel = `Slot ${info.active_slot + 1}`;
    let emulationDetail = 'Enabled NFC and RFID places are available for emulation';
    if (info.busy) {
      slotLabel = 'Device busy';
      emulationDetail = 'A reader operation owns the device';
    } else if (info.reader) {
      slotLabel = 'Reader mode';
      emulationDetail = 'Ready for a reader operation';
    }

    $('nav-slot-title').textContent = slotLabel;
    $('nav-slot-detail').textContent = frequencies || 'tags disabled';
    $('nav-slot-detail').hidden = info.busy || info.reader;
    $('nav-slot').dataset.state = !info.busy && !info.reader && places.length ? 'emulating' : 'idle';
    $('emulation-state').className = `emulation-state${info.reader ? '' : ' active'}`;
    $('emulation-title').textContent = info.reader ? 'Reader mode' : `Slot ${info.active_slot + 1} selected`;
    $('emulation-detail').textContent = emulationDetail;
    $('device-info').innerHTML = `
      <div class="cu-hero-top">
        <div>
          <strong class="cu-model">Chameleon Ultra</strong>
          <span class="cu-sub">Firmware v${escape(info.version)} · USB</span>
        </div>
        <span class="cu-mode">${info.reader ? 'Reader' : 'Emulator'}</span>
      </div>
      <div class="device-list">
        <div class="device-row">
          <span>Battery</span>
          <strong>${info.battery_percent}% · ${(info.battery_mv / 1000).toFixed(2)} V</strong>
        </div>
        <div class="device-row"><span>Active slot</span><strong>${info.active_slot + 1}</strong></div>
        <div class="device-row"><span>Build</span><strong>${escape(info.git)}</strong></div>
        <div class="device-row"><span>Chip ID</span><strong><code>${escape(info.chip)}</code></strong></div>
      </div>`;

    const select = (key, label, options) => {
      const choices = options
        .map(([value, name]) => {
          const selected = Number(settings[key]) === value ? ' selected' : '';
          return `<option value="${value}"${selected}>${name}</option>`;
        })
        .join('');
      return `<label class="field">${label}<select data-setting="${key}">${choices}</select></label>`;
    };

    const field = (key, label, min, max) =>
      `<label class="field">${label}<input data-setting="${key}" type="number"
        min="${min}" max="${max}" value="${settings[key]}"></label>`;

    const buttonSettings = [
      ['press_a', 'A · press'],
      ['press_b', 'B · press'],
      ['long_a', 'A · long press'],
      ['long_b', 'B · long press']
    ]
      .map(([key, label]) =>
        select(
          key,
          label,
          ACTIONS.map((label, i) => [i, label])
        )
      )
      .join('');
    const animation = select('animation', 'LED animation', [
      [0, 'Full'],
      [1, 'Minimal'],
      [3, 'Symmetric'],
      [2, 'None']
    ]);
    const pairing = select('pairing', 'Require pairing', [
      [0, 'Off'],
      [1, 'On']
    ]);

    $('device-settings').innerHTML = `
      <div class="card">
        <h3 class="cu-h">Buttons</h3>
        <div class="cu-grid">
          ${buttonSettings}
          ${field('long_ms', 'Long press after · ms', 200, 65535)}
          ${field('sleep', 'Sleep after · seconds', 5, 60)}
        </div>
      </div>
      <div class="card">
        <h3 class="cu-h">LEDs &amp; Bluetooth</h3>
        <div class="cu-grid">
          ${animation}
          ${pairing}
          <label class="field">New pairing PIN<input data-setting="pin" inputmode="numeric"
            maxlength="6" placeholder="6 digits" autocomplete="off"></label>
        </div>
        <p class="memory-hint">Pairing changes may require a device restart.</p>
      </div>`;
  }

  async function load() {
    try {
      const next = await request('/api/device');
      if (
        !next.settings ||
        !Number.isInteger(next.active_slot) ||
        next.active_slot < 0 ||
        next.active_slot > 7 ||
        !Number.isInteger(next.battery_percent) ||
        next.battery_percent < 0 ||
        next.battery_percent > 100
      ) {
        throw new Error('Invalid device response');
      }

      next.slot = decodeSlot(await request(`/api/slot/${next.active_slot}`));
      info = next;
      render();

      onConnection(true, info);
      return { ok: true };
    } catch (error) {
      for (const button of document.querySelectorAll('[data-connection]')) {
        button.textContent = 'Offline';
        button.removeAttribute('aria-label');
        button.dataset.state = 'offline';
      }

      $('nav-slot-title').textContent = 'Device unavailable';
      $('nav-slot-detail').hidden = true;
      $('nav-slot').dataset.state = 'offline';
      $('emulation-state').className = 'emulation-state';
      $('emulation-title').textContent = 'Device unavailable';
      $('emulation-detail').textContent = error.message;

      onConnection(false);
      return { ok: false, error };
    }
  }

  function refresh() {
    if (!refreshPromise) {
      refreshPromise = load().finally(() => {
        refreshPromise = null;
      });
    }
    return refreshPromise;
  }

  $('device-settings').onchange = async (event) => {
    const field = event.target;
    const key = field.dataset.setting;
    if (!key) {
      return;
    }

    const value = field.value.trim();
    let payload;
    if (key === 'pin') {
      if (!/^\d{6}$/.test(value)) {
        notify('The pairing PIN must be exactly 6 digits.', true);
        return;
      }
      payload = [8, ...new TextEncoder().encode(value)];
    } else {
      const id = SETTINGS.indexOf(key);
      const number = Number(value);
      if (id < 0 || !/^\d+$/.test(value) || !field.checkValidity()) {
        notify('Enter a value within the allowed range.', true);
        return;
      }
      payload = id === 6 ? [id, number >> 8, number & 255] : [id, number];
    }

    field.disabled = true;
    try {
      const bytes = await request('/api/settings', new Uint8Array(payload));
      if (bytes.length !== 2 || bytes[0] !== 0 || bytes[1] !== 0x68) {
        throw new Error(`Settings save unconfirmed (${hex(bytes)})`);
      }
      notify('Saved on the device.');
    } catch (error) {
      notify(error.message, true);
    } finally {
      field.disabled = false;
      await refresh();
    }
  };

  return { refresh };
}
