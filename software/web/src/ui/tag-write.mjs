import { maintainClassic } from '../device/classic-maintenance.mjs';
import { hex } from '../device/transport.mjs';
import { magicGen1aDetected, magicGen2Confirmed } from '../documents/classic-magic.mjs';
import {
  copyToTag as copyToDevice,
  changeTagUid as changeUidOnDevice,
  writeType2Changes as writeType2ChangesToDevice,
  writeClassicChanges as writeChangesToDevice
} from '../device/tag-write.mjs';
import { ask } from './dialogs.mjs';
import { $ } from './device-view.mjs';
import { copyLfToTag } from './lf-copy.mjs';

export function writeClassicChanges(source, signal, onProgress) {
  return writeChangesToDevice(source, signal, onProgress, {
    confirmChanges: ({ uid, data, trailers }) => {
      let message = `Tag ${hex(uid)}`;
      if (data.length) {
        message += ` · ${data.length} data block${data.length === 1 ? '' : 's'}: ${data.map((unit) => unit.index).join(', ')}.`;
      }
      if (trailers.length) {
        message += `\nKeys and access rights: sector${trailers.length === 1 ? '' : 's'} ${trailers.map((unit) => unit.sector).join(', ')}.\nThe edited trailers replace both keys and access rights. Read the tag again after writing.`;
      }
      return ask({ title: 'Write changes?', message, confirmLabel: 'Write changes', signal });
    }
  });
}

export function writeType2Changes(source, signal, onProgress) {
  return writeType2ChangesToDevice(source, signal, onProgress, {
    requestCredential: optionalCredential,
    confirmChanges: ({ uid, writes }) => {
      const pages = writes.length === 1 ? 'page' : 'pages';
      const message = `Tag ${hex(uid)} · ${writes.length} user ${pages}: ${writes.map((unit) => unit.index).join(', ')}.`;
      return ask({
        title: 'Write page changes?',
        message,
        confirmLabel: 'Write changes',
        signal
      });
    }
  });
}

function panel(title, message, action, detail = '') {
  const dialog = document.createElement('dialog');
  dialog.className = 'app-dialog';

  const heading = document.createElement('h2');
  heading.id = 'physical-dialog-title';
  heading.textContent = title;
  dialog.setAttribute('aria-labelledby', heading.id);

  const copy = document.createElement('p');
  copy.textContent = message;

  const details = document.createElement('p');
  details.className = 'memory-hint';
  details.textContent = detail;

  const buttons = document.createElement('div');
  buttons.className = 'dialog-actions';

  const cancel = document.createElement('button');
  cancel.className = 'secondary';
  cancel.textContent = 'Cancel';

  const confirm = document.createElement('button');
  confirm.className = 'primary-action';
  confirm.textContent = action;

  buttons.append(cancel, confirm);
  dialog.append(heading, copy, details, buttons);
  document.body.append(dialog);

  return { dialog, confirm, cancel, details, copy };
}

function choose(view, signal) {
  return new Promise((resolve) => {
    const finish = (accepted) => {
      signal?.removeEventListener('abort', abort);
      view.dialog.close();
      view.dialog.remove();
      resolve(accepted);
    };

    view.confirm.onclick = () => finish(true);
    view.cancel.onclick = () => finish(false);
    view.dialog.oncancel = (event) => {
      event.preventDefault();
      finish(false);
    };

    const abort = () => finish(false);
    if (signal?.aborted) {
      finish(false);
      return;
    }
    signal?.addEventListener('abort', abort, { once: true });

    view.dialog.showModal();
    view.confirm.focus();
  });
}

async function optionalCredential(ultralightC, signal) {
  const label = ultralightC ? '3DES key' : 'password';
  const digits = ultralightC ? 32 : 8;
  const view = panel(
    'Protected pages',
    'Some target pages are protected or could not be read.',
    `Use ${label}`,
    `Enter the target tag ${label} to check those pages, or continue with a partial copy.`
  );
  view.cancel.textContent = 'Continue without';

  const input = document.createElement('input');
  input.className = 'dialog-input';
  input.type = 'password';
  input.autocomplete = 'off';
  input.setAttribute('aria-label', `Target ${label}`);
  input.placeholder = `${digits} hex digits`;
  input.maxLength = digits;
  view.dialog.insertBefore(input, view.dialog.lastChild);

  input.oninput = () => {
    view.confirm.disabled = input.value.length !== digits || !/^[0-9a-f]+$/i.test(input.value);
  };

  input.oninput();

  return (await choose(view, signal))
    ? Array.from(input.value.match(/../g), (byte) => parseInt(byte, 16))
    : null;
}

async function confirmCopy({ sourceUid, targetUid, unitLabel, writes, unchanged, omissions, clone }, signal) {
  const view = panel(
    'Copy to a tag',
    'Keep the target on the antenna until verification finishes.',
    'Copy data'
  );
  view.dialog.classList.add('copy-dialog');
  const identities = document.createElement('dl');
  identities.className = 'copy-identities';
  for (const [label, uid] of [
    ['Source UID', sourceUid],
    ['Target UID', targetUid]
  ]) {
    const term = document.createElement('dt');
    term.textContent = label;
    const value = document.createElement('dd');
    value.textContent = hex(uid);
    identities.append(term, value);
  }
  view.dialog.insertBefore(identities, view.details);

  let selected = 'data';
  const summary = () => {
    const cloning = selected === 'clone';
    view.confirm.textContent = cloning ? 'Clone tag' : 'Copy data';
    view.confirm.disabled = cloning ? !clone?.writes || !!clone.reason : !writes;
    if (cloning) {
      if (!clone.writes) {
        view.details.textContent = 'The target already matches the source. No writes are needed.';
        return;
      }
      const sectors = clone.trailers.map((unit) => unit.sector).join(', ');
      view.details.textContent = [
        `${clone.writes} blocks to write.`,
        clone.identity
          ? 'Replace the complete block 0, including the UID and stored SAK/ATQA bytes.'
          : 'Block 0 already matches.',
        sectors
          ? `Replace keys and access rights in sectors ${sectors}.`
          : 'Keys and access rights already match.',
        'Use a known rewritable target; some one-time variants cannot be identified before writing.'
      ].join('\n');
    } else {
      const plural = writes === 1 ? '' : 's';
      view.details.textContent = [
        `${writes} data ${unitLabel}${plural} to write · ${unchanged} already match.`,
        'UID, keys and access rights stay unchanged on the target.'
      ].join('\n');
      if (omissions.length) {
        view.details.textContent += `\nNot copied: ${omissions.join(', ')}.`;
      }
    }
  };
  view.details.setAttribute('role', 'status');
  view.details.setAttribute('aria-live', 'polite');

  if (clone) {
    const modes = document.createElement('fieldset');
    modes.className = 'copy-modes';
    const legend = document.createElement('legend');
    legend.textContent = 'What to copy';
    modes.append(legend);
    for (const [mode, title, description] of [
      ['data', 'Copy data', 'Keep the target UID, keys and access rights.'],
      ['clone', 'Clone tag', 'Match data, full block 0, keys and access rights.']
    ]) {
      const label = document.createElement('label');
      label.className = 'copy-mode';
      const input = document.createElement('input');
      input.type = 'radio';
      input.name = 'physical-copy-mode';
      input.value = mode;
      input.checked = mode === 'data';
      input.disabled = mode === 'clone' && !!clone.reason;
      const text = document.createElement('span');
      const heading = document.createElement('span');
      heading.className = 'copy-mode-title';
      const name = document.createElement('strong');
      name.textContent = title;
      heading.append(name);
      const hint = document.createElement('span');
      hint.className = 'copy-mode-description';
      hint.textContent = description;
      text.append(heading, hint);
      if (input.disabled) {
        const badge = document.createElement('span');
        badge.className = 'copy-mode-badge';
        badge.textContent = 'Unavailable';
        heading.append(badge);
        const reason = document.createElement('span');
        reason.className = 'copy-unavailable';
        reason.textContent = clone.reason;
        text.append(reason);
      }
      input.onchange = () => {
        selected = mode;
        summary();
      };
      label.append(input, text);
      modes.append(label);
    }
    view.dialog.insertBefore(modes, view.details);
  }
  summary();
  return (await choose(view, signal)) ? selected : null;
}

async function requestUid({ card, magic, credential, suggestedUid }, signal) {
  const gen1a = magicGen1aDetected(magic);
  let detail =
    'UID writability is unconfirmed. A block-0 write may be one-time on some tags. Enter the sector-0 key only if you intend that write.';
  if (gen1a) {
    detail = 'Gen1A detected. Enter the new four-byte UID. No key is required. Other tag data is preserved.';
  } else if (magicGen2Confirmed(magic)) {
    detail =
      'CUID detected. Enter the new four-byte UID and use the sector-0 key. Other tag data is preserved.';
  }
  const view = panel('Change tag UID', `Current UID: ${hex(card.uid)}`, 'Change UID', detail);
  if (!gen1a && !magicGen2Confirmed(magic)) {
    view.confirm.textContent = 'Write UID with key';
  }

  const input = document.createElement('input');
  input.className = 'dialog-input';
  input.setAttribute('aria-label', 'New UID');
  input.placeholder = '01020304';
  input.maxLength = 8;
  if (suggestedUid?.length === 4) {
    input.value = hex(suggestedUid);
  }
  view.dialog.insertBefore(input, view.dialog.lastChild);

  let keyInput;
  let keyType;
  if (!gen1a) {
    keyType = document.createElement('select');
    keyType.className = 'dialog-input';
    keyType.setAttribute('aria-label', 'Key type');
    for (const [value, label] of [
      [1, 'Key A'],
      [2, 'Key B']
    ]) {
      const option = document.createElement('option');
      option.value = value;
      option.textContent = label;
      keyType.append(option);
    }
    keyInput = document.createElement('input');
    keyInput.className = 'dialog-input';
    keyInput.setAttribute('aria-label', 'Sector-0 key');
    keyInput.placeholder = 'FFFFFFFFFFFF';
    keyInput.maxLength = 12;
    if (credential) {
      keyType.value = credential.keyB ? '2' : '1';
      keyInput.value = hex(credential.key);
    }
    view.dialog.insertBefore(keyType, view.dialog.lastChild);
    view.dialog.insertBefore(keyInput, view.dialog.lastChild);
    keyInput.oninput = () => input.oninput();
  }

  input.oninput = () => {
    const invalidUid = !/^[0-9a-f]{8}$/i.test(input.value);
    const unchangedUid = input.value.toUpperCase() === hex(card.uid);
    const invalidKey = keyInput && !/^[0-9a-f]{12}$/i.test(keyInput.value);
    view.confirm.disabled = invalidUid || unchangedUid || invalidKey;
  };

  input.oninput();
  if (!(await choose(view, signal))) {
    return null;
  }

  const uid = Array.from(input.value.match(/../g), (value) => parseInt(value, 16));
  const key = keyInput ? Array.from(keyInput.value.match(/../g), (value) => parseInt(value, 16)) : null;
  return { uid, key, keyB: keyType?.value === '2' };
}

export async function copyToTag(source, signal, onProgress = () => {}) {
  if (source.kind === 'lf') {
    return copyLfToTag(source, signal, onProgress);
  }

  const feedback = $('copy-progress');
  const label = $('copy-progress-label');
  const bar = $('copy-progress-bar');
  const startedAt = performance.now();
  let outcome = 'success';

  function setState(value) {
    feedback.dataset.state = value;
    feedback.setAttribute('aria-busy', String(value === 'busy'));
  }

  function showTime() {
    $('copy-elapsed').textContent = `${((performance.now() - startedAt) / 1000).toFixed(1)} s`;
  }

  function progress(message, detail) {
    onProgress(message);
    if (detail?.phase === 'cancelled') {
      outcome = 'stopped';
    }
    if (detail?.total) {
      feedback.classList.remove('indeterminate');
      bar.max = detail.total;
      bar.value = detail.completed;
      label.textContent = `${detail.completed} / ${detail.total} ${detail.unitLabel}${detail.total === 1 ? '' : 's'} verified`;
    }
  }

  async function decision(action, ...args) {
    setState('paused');
    try {
      return await action(...args);
    } finally {
      setState('busy');
    }
  }

  feedback.hidden = false;
  feedback.classList.add('indeterminate');
  bar.removeAttribute('value');
  label.textContent = 'Checking target…';
  setState('busy');
  showTime();
  const interval = setInterval(showTime, 100);
  try {
    await copyToDevice(source, signal, progress, {
      requestCredential: (...args) => decision(optionalCredential, ...args),
      confirmCopy: (...args) => decision(confirmCopy, ...args),
      dictionary: $('dictionary')?.value || ''
    });
    if (signal?.aborted) {
      outcome = 'stopped';
      onProgress('Copy stopped · completed writes were verified');
    }
  } catch (error) {
    outcome = signal?.aborted ? 'stopped' : 'error';
    throw error;
  } finally {
    clearInterval(interval);
    showTime();
    setState(outcome);
    const titles = { success: 'Copy verified', stopped: 'Copy stopped', error: 'Copy failed' };
    label.textContent = bar.hasAttribute('value')
      ? `${titles[outcome]} · ${label.textContent}`
      : titles[outcome];
  }
}

export function changeTagUid(signal, onProgress, source) {
  return changeUidOnDevice(signal, onProgress, source, { requestUid });
}

export async function maintainTag(format, source, signal, onProgress) {
  const view = panel(
    format ? 'Format MIFARE Classic' : 'Repair MIFARE Classic',
    'Keep the physical card on the antenna until verification finishes.',
    'Checking card…'
  );
  view.details.setAttribute('role', 'status');
  view.dialog.insertBefore($('maintenance-template').content.cloneNode(true), view.details);
  const settings = view.dialog.querySelector('fieldset');
  const fields = Object.fromEntries([...settings.elements].map((input) => [input.name, input]));
  settings.querySelector('[data-repair-options]').hidden = format;
  const manual = settings.querySelector('[data-manual]');
  const credentials = settings.querySelector('[data-credentials]');
  const sectors = settings.querySelector('[data-sectors]');
  const preview = view.dialog.querySelector('.maintenance-preview');
  view.dialog.insertBefore(preview, settings);
  const result = view.dialog.querySelector('.maintenance-result');
  view.dialog.insertBefore(result, view.copy);
  const effectDetails = preview.querySelector('[data-effect-details]');

  const controller = new AbortController();
  let decision;
  let running = false;
  let recheck = false;
  let resolveDialog;
  const close = (written = false) => {
    signal?.removeEventListener('abort', cancel);
    view.dialog.close();
    view.dialog.remove();
    resolveDialog(written);
  };
  const cancel = () => {
    controller.abort();
    decision?.(false);
    if (!running) {
      close();
    }
  };
  const changed = () => {
    sectors.hidden = !fields.resetAccess.checked;
    if (decision) {
      recheck = true;
      view.confirm.disabled = true;
      preview.hidden = true;
      result.hidden = true;
      view.copy.hidden = false;
      view.details.hidden = false;
      view.cancel.hidden = false;
      decision(false);
      decision = null;
    }
  };
  settings.oninput = changed;
  signal?.addEventListener('abort', cancel, { once: true });
  view.cancel.onclick = cancel;
  view.dialog.oncancel = (event) => {
    event.preventDefault();
    cancel();
  };
  const bytes = (input, sizes) => {
    const value = input?.value.trim() || '';
    if (!value) {
      return null;
    }
    if (!sizes.includes(value.length) || !/^[0-9a-f]+$/i.test(value)) {
      throw new Error(`Check ${input.getAttribute('aria-label')}: enter hexadecimal bytes.`);
    }
    return value.match(/../g).map((byte) => parseInt(byte, 16));
  };
  return new Promise((resolve) => {
    resolveDialog = resolve;
    view.confirm.onclick = async () => {
      if (decision) {
        const accept = decision;
        decision = null;
        settings.disabled = true;
        view.confirm.disabled = true;
        accept(true);
        return;
      }
      try {
        const options = {
          repairIdentity: format || fields.repairIdentity.checked,
          method: format ? 0 : Number(fields.method.value),
          model: format ? 0 : Number(fields.model.value),
          key: bytes(fields.key, [12]),
          uid: format ? null : bytes(fields.uid, [8, 14]),
          sectors: !format && fields.resetAccess.checked ? fields.sectors.value.trim() : '',
          dictionary: $('dictionary').value
        };
        if (!format && fields.resetAccess.checked && !options.sectors) {
          throw new Error('Enter sectors to reset, or all.');
        }
        if (!format && !options.repairIdentity && !options.sectors) {
          throw new Error('Choose identification repair or sectors whose keys and access should be reset.');
        }
        running = true;
        recheck = false;
        settings.disabled = true;
        view.confirm.disabled = true;
        preview.hidden = true;
        result.hidden = true;
        const written = await maintainClassic(
          format,
          source,
          options,
          controller.signal,
          (message) => {
            view.details.textContent = message;
            onProgress(message);
          },
          ({ card, method, geometry, plan, reset }) => {
            const identity = plan.find((unit) => unit.index === 0);
            const data = plan.filter((unit) => !unit.trailer && unit.index !== 0);
            const trailers = plan.filter((unit) => unit.trailer);
            preview.querySelector('.maintenance-target').textContent =
              `${geometry.name} · UID ${hex(card.uid)}`;
            const effects = {
              data: format ? 'Erased (00)' : 'Preserved',
              keys: format || reset.length ? 'FFFFFFFFFFFF' : 'Preserved',
              access: format || reset.length ? 'Factory defaults' : 'Preserved',
              identity: identity ? 'Identification corrected; manufacturer tail preserved' : 'Preserved'
            };
            if (identity && options.uid) {
              effects.identity = `UID → ${hex(options.uid)}; manufacturer tail preserved`;
            }
            if (format && !plan.length) {
              effects.data = 'Already zero';
            }
            for (const [name, value] of Object.entries(effects)) {
              preview.querySelector(`[data-effect="${name}"]`).textContent = value;
            }
            const changes = preview.querySelector('[data-changes]');
            if (format) {
              const dataLabel = data.length === 1 ? 'data block' : 'data blocks';
              const sectorLabel = trailers.length === 1 ? 'sector' : 'sectors';
              const erase = data.length ? `${data.length} ${dataLabel} to erase` : 'Data already zero';
              changes.textContent = `${erase} · reset ${trailers.length} ${sectorLabel}`;
              if (!plan.length) {
                changes.textContent = 'Card already formatted · no writes needed';
              }
            } else {
              changes.textContent = [
                identity ? 'Correct identification (block 0)' : 'Identification unchanged',
                reset.length ? `Reset sectors ${reset.join(', ')}` : ''
              ]
                .filter(Boolean)
                .join(' · ');
            }
            preview.querySelector('[data-technical]').textContent = [
              `Method: ${['', 'Gen1A backdoor', 'Gen1B backdoor', 'Authenticated Classic writing'][method]}`,
              `${plan.length} blocks to write: ${plan.map((unit) => unit.index).join(', ') || 'none'}`,
              format || reset.length
                ? `Access/GPB: FF078069${format ? ' (FF0780BC in the final sector)' : ''}`
                : '',
              identity ? `Block 0: ${hex(identity.before)} → ${hex(identity.after)}` : '',
              !format && options.repairIdentity
                ? 'Use a known rewritable card; some one-time variants share its signatures.'
                : ''
            ]
              .filter(Boolean)
              .join('\n');
            preview.hidden = false;
            result.hidden = !!plan.length;
            result.querySelector('[data-result-title]').textContent = format
              ? 'Already formatted'
              : 'No repair needed';
            result.querySelector('[data-result-message]').textContent =
              `${format ? '' : 'For the selected options. '}No writes needed. The card has not been modified.`;
            effectDetails.open = !!plan.length;
            effectDetails.querySelector('summary').hidden = !!plan.length;
            view.copy.hidden = !plan.length;
            view.details.hidden = !plan.length;
            view.cancel.hidden = !plan.length;
            settings.hidden = format && !plan.length;
            view.details.textContent = plan.length ? 'Ready · no writes sent' : '';
            view.confirm.textContent = 'Close';
            if (plan.length) {
              view.confirm.textContent = format ? 'Format card' : 'Apply repair';
            }
            view.confirm.disabled = false;
            settings.disabled = false;
            if (!plan.length) {
              view.confirm.focus({ preventScroll: true });
            }
            return new Promise((resolve) => {
              decision = resolve;
            });
          }
        );
        if (recheck && !controller.signal.aborted) {
          view.details.textContent = 'Options changed · check the card again before writing.';
          view.confirm.textContent = 'Check changes';
        } else {
          close(written);
        }
      } catch (error) {
        if (controller.signal.aborted) {
          onProgress(`Maintenance stopped · ${error.message}`);
          close();
        } else {
          view.details.textContent = error.message;
          onProgress(error.message);
          if (error.setup && !format) {
            manual.hidden = false;
            manual.open = true;
          } else {
            credentials.open = true;
          }
          view.confirm.textContent = 'Check again';
        }
      } finally {
        running = false;
        settings.disabled = false;
        view.confirm.disabled = false;
      }
    };
    if (signal?.aborted) {
      close();
      return;
    }
    view.dialog.showModal();
    view.cancel.focus();
    view.confirm.click();
  });
}
