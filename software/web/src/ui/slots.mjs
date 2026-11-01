import { request, withDevice } from '../device/transport.mjs';
import { SlotDevice, decodeSlot } from '../device/slots.mjs';
import { ask } from './dialogs.mjs';
import { $, escape } from './device-view.mjs';
import { be32, family, nickname, typeName } from '../documents/slot-image.mjs';

export function slotView({ run, open, add, notify, afterChange }) {
  let active = 0;
  let rows = [];
  let refreshPromise;

  async function load() {
    const nextActive = (await request('/api/device')).active_slot;
    const next = [];
    for (let i = 0; i < 8; i++) {
      next.push(decodeSlot(await request(`/api/slot/${i}`)));
    }

    // Keep focus and in-flight clicks intact when a refresh changes nothing.
    if (active === nextActive && JSON.stringify(rows) === JSON.stringify(next)) {
      return;
    }

    active = nextActive;
    rows = next;
    render();
  }

  function refresh() {
    if (!refreshPromise) {
      refreshPromise = load().finally(() => {
        refreshPromise = null;
      });
    }
    return refreshPromise;
  }

  async function mutate(work, message) {
    await run(async (signal) => {
      const device = new SlotDevice(signal);
      let error;
      try {
        await withDevice(device, async () => {
          await device.begin();
          await work(device);
        });
      } catch (caught) {
        error = caught;
      }

      try {
        await refresh();
        await afterChange();
      } catch (refreshError) {
        error ??= refreshError;
      }

      if (error) {
        throw error;
      }
      if (message) {
        notify(message);
      }
    });
  }

  function button(label, action, className = 'mini-action') {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = className;
    button.textContent = label;
    button.onclick = () => Promise.resolve(action()).catch((error) => notify(error.message, true));
    return button;
  }

  function render() {
    $('slot-strip').replaceChildren();
    $('slots').replaceChildren();

    rows.forEach((row, slotIndex) => {
      const hfEnabled = row.hf.type && row.hf.enabled;
      const lfEnabled = row.lf.type && row.lf.enabled;
      let led = 'off';
      if (hfEnabled && lfEnabled) {
        led = 'red';
      } else if (hfEnabled) {
        led = 'green';
      } else if (lfEnabled) {
        led = 'blue';
      }

      const select = () =>
        mutate((device) =>
          device.action('activate', [slotIndex, ...be32(row.hf.revision), ...be32(row.lf.revision)])
        );
      const selected = slotIndex === active;
      const tagsOff = !hfEnabled && !lfEnabled;
      const tagsOffClass = tagsOff ? ' tags-off' : '';
      const selectionClass = selected ? ` active${tagsOffClass}` : '';
      const strip = button(String(slotIndex + 1), select, `slot-led-btn${selectionClass}`);
      strip.innerHTML = `<span class="slot-led" data-led="${led}"></span><small>${slotIndex + 1}</small>`;
      let selectionLabel = '';
      if (selected) {
        selectionLabel = tagsOff ? ', selected, NFC and RFID disabled' : ', selected';
      }
      strip.setAttribute('aria-label', `Slot ${slotIndex + 1}${selectionLabel}`);
      strip.setAttribute('aria-pressed', String(selected));
      $('slot-strip').append(strip);

      const card = document.createElement('article');
      card.className = `slot-card${selectionClass}`;
      card.id = `slot-${slotIndex}`;

      const head = document.createElement('header');
      head.className = 'slot-head';
      head.innerHTML = `<span class="slot-led" data-led="${led}"></span><strong>Slot ${slotIndex + 1}</strong>`;
      if (selected) {
        const selectionStatus = tagsOff ? 'Selected · NFC and RFID disabled' : 'Selected';
        head.insertAdjacentHTML(
          'beforeend',
          `<span class="slot-active${tagsOffClass}">${selectionStatus}</span>`
        );
      } else {
        head.append(button('Select slot', select));
      }
      card.append(head);

      for (const [sense, place] of Object.entries(row)) {
        const code = sense === 'hf' ? 2 : 1;
        const frequency = sense === 'hf' ? 'NFC' : 'RFID';
        const bay = document.createElement('div');
        bay.className = `slot-bay${place.enabled ? '' : ' off'}`;
        bay.innerHTML = `<span class="bay-chip ${sense}">${frequency}</span>`;

        const title = button(
          '',
          () => {
            if (place.type) {
              return open(slotIndex, code, place);
            }
            return add(slotIndex, code);
          },
          'bay-open'
        );
        const typeLabel = place.type ? typeName(place.type) : '＋ Import a tag…';
        let description = 'Choose a file';
        if (place.type) {
          description = escape(typeLabel) + (place.enabled ? '' : ' · off');
        }
        title.innerHTML = `<strong>${escape(place.name || typeLabel)}</strong><small>${description}</small>`;
        title.disabled = !!place.type && !family(place.type);
        bay.append(title);
        if (place.type) {
          const rename = button(
            '✎',
            async () => {
              const name = await ask({
                title: `Name for slot ${slotIndex + 1} · ${frequency}`,
                message: '32 UTF-8 bytes maximum.',
                initial: place.name,
                confirmLabel: 'Rename'
              });
              if (name === null) {
                return;
              }
              const bytes = nickname(name);
              await mutate(
                (device) => device.action('nick', [slotIndex, code, ...be32(place.revision), ...bytes]),
                `Slot ${slotIndex + 1} ${frequency} renamed`
              );
            },
            'bay-rename'
          );
          rename.innerHTML = `
            <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor"
              stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
              <path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M13 7l4 4"/>
            </svg>`;
          rename.setAttribute('aria-label', `Rename slot ${slotIndex + 1} ${sense}`);
          bay.append(rename);

          const toggle = button(
            '',
            () =>
              mutate((device) =>
                device.action('enable', [slotIndex, code, ...be32(place.revision), +!place.enabled])
              ),
            'bay-switch'
          );
          toggle.setAttribute('role', 'switch');
          toggle.setAttribute('aria-checked', String(place.enabled));
          toggle.setAttribute('aria-label', `Enable slot ${slotIndex + 1} ${sense}`);
          toggle.innerHTML = '<i></i>';
          bay.append(toggle);

          const clear = button(
            '×',
            async () => {
              if (
                !(await ask({
                  title: `Clear slot ${slotIndex + 1} ${frequency}?`,
                  message: 'Export its image first if you want to keep it.',
                  confirmLabel: 'Clear'
                }))
              ) {
                return;
              }
              await mutate(
                (device) => device.action('clear', [slotIndex, code, ...be32(place.revision)]),
                `Slot ${slotIndex + 1} ${frequency} cleared`
              );
            },
            'bay-clear'
          );
          clear.setAttribute('aria-label', `Clear slot ${slotIndex + 1} ${sense}`);
          bay.append(clear);
        }
        card.append(bay);
      }
      $('slots').append(card);
    });
  }

  return {
    refresh,
    mutate,
    async choose(sense, index) {
      await refresh();
      if (index !== undefined) {
        return { slot: index, place: rows[index][sense === 2 ? 'hf' : 'lf'] };
      }
      return chooseSlot(rows, sense);
    }
  };
}

function chooseSlot(rows, sense) {
  const dialog = document.createElement('dialog');
  dialog.className = 'app-dialog slot-sheet';
  dialog.setAttribute('aria-label', 'Choose a slot');
  dialog.innerHTML = `
    <h2>Save to which slot?</h2>
    <div class="slot-sheet-list"></div>
    <button class="secondary" type="button">Cancel</button>`;
  let selected = null;

  rows.forEach((row, slotIndex) => {
    const place = row[sense === 2 ? 'hf' : 'lf'];
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'slot-option';
    const description = place.type
      ? `Replaces “${escape(place.name || typeName(place.type))}”`
      : 'Empty place';
    button.innerHTML = `<span><strong>Slot ${slotIndex + 1}</strong><small>${description}</small></span>`;
    button.onclick = () => {
      selected = { slot: slotIndex, place };
      dialog.close();
    };
    dialog.querySelector('.slot-sheet-list').append(button);
  });

  dialog.querySelector('.secondary').onclick = () => dialog.close();
  document.body.append(dialog);
  return new Promise((resolve) => {
    dialog.addEventListener(
      'close',
      () => {
        dialog.remove();
        resolve(selected);
      },
      { once: true }
    );

    dialog.showModal();
  });
}
