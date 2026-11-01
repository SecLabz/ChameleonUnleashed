import { Device } from '../device/device.mjs';
import { withDevice } from '../device/transport.mjs';
import { prepareLfCopy, programLfCopy } from '../device/lf-program.mjs';
import { renderLfChip } from './lf-chip.mjs';
import { lfIdentity } from '../documents/lf.mjs';
import { encodeSlotImage, typeName } from '../documents/slot-image.mjs';

export async function copyLfToTag(source, signal, onProgress) {
  if (signal.aborted) {
    return;
  }
  // Freeze the document before the user places a different tag on the antenna.
  const frozen = structuredClone(source);
  encodeSlotImage(frozen);
  const controller = new AbortController();
  const dialog = document.createElement('dialog');
  dialog.className = 'app-dialog';
  dialog.setAttribute('aria-labelledby', 'lf-program-title');
  const form = document.createElement('form');
  const heading = document.createElement('h2');
  heading.id = 'lf-program-title';
  const message = document.createElement('p');
  message.setAttribute('role', 'status');
  message.setAttribute('aria-live', 'polite');
  const chip = document.createElement('div');
  chip.className = 'lf-chip';
  chip.hidden = true;
  const summary = document.createElement('dl');
  summary.className = 'lf-plan';
  for (const [label, text] of [
    ['Format', typeName(frozen.type)],
    ['New ID', lfIdentity(frozen).replace(/^ID /, '')]
  ]) {
    const term = document.createElement('dt');
    term.textContent = label;
    const value = document.createElement('dd');
    value.textContent = text;
    summary.append(term, value);
  }
  const detail = document.createElement('p');
  detail.className = 'memory-hint';
  const actions = document.createElement('div');
  actions.className = 'dialog-actions lf-program-actions';
  const cancel = document.createElement('button');
  cancel.type = 'button';
  cancel.className = 'secondary';
  const confirm = document.createElement('button');
  confirm.type = 'submit';
  confirm.className = 'primary-action';
  actions.append(cancel, confirm);
  form.append(heading, message, chip, summary, detail, actions);
  dialog.append(form);
  let resolveChoice;

  function stop() {
    controller.abort();
    resolveChoice?.(false);
    dialog.close();
  }

  function choose(title, text, label) {
    if (controller.signal.aborted) {
      return Promise.resolve(false);
    }
    heading.textContent = title;
    message.textContent = text;
    message.hidden = !text;
    cancel.textContent = 'Cancel';
    confirm.textContent = label;
    confirm.disabled = false;
    return new Promise((resolve) => {
      resolveChoice = resolve;
      cancel.focus();
    });
  }

  function progress(text) {
    resolveChoice = null;
    confirm.disabled = true;
    cancel.textContent = 'Stop';
    message.textContent = text;
    message.hidden = false;
    onProgress(text);
  }

  cancel.onclick = stop;
  dialog.oncancel = (event) => {
    event.preventDefault();
    stop();
  };
  form.onsubmit = (event) => {
    event.preventDefault();
    if (!confirm.disabled) {
      resolveChoice?.(true);
      resolveChoice = null;
      confirm.disabled = true;
    }
  };
  signal.addEventListener('abort', stop, { once: true });
  document.body.append(dialog);
  dialog.showModal();

  try {
    detail.textContent = 'A T55x7 target with password mode disabled is required.';
    if (!(await choose('Program a tag', 'Place the target tag on the LF antenna.', 'Check target'))) {
      return;
    }
    const device = new Device(controller.signal);
    return await withDevice(device, async () => {
      progress('Checking target chip, configuration and readable blocks…');
      await device.reserve();
      const plan = await prepareLfCopy(device, frozen);
      if (controller.signal.aborted) {
        onProgress('Target check stopped · nothing written');
        return;
      }
      if (!plan.changes) {
        onProgress('Target already matches · verified without writing');
        return false;
      }
      detail.textContent = `${plan.changes} ${plan.changes === 1 ? 'block will' : 'blocks will'} change. Keep this tag on the antenna until verification finishes.`;
      chip.hidden = false;
      renderLfChip(chip, plan.chip);
      if (!(await choose('Program this target?', '', 'Program & verify'))) {
        onProgress('Programming cancelled · nothing written');
        return;
      }
      heading.textContent = 'Programming tag';
      progress('Programming and verifying target… Keep it on the antenna.');
      await programLfCopy(device, plan);
      onProgress('RFID programmed · T55xx blocks written and verified');
      return true;
    });
  } finally {
    signal.removeEventListener('abort', stop);
    dialog.close();
    dialog.remove();
  }
}
