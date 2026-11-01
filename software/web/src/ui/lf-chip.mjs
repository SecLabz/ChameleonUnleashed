import { lfChipInfo } from '../device/lf-program.mjs';

export function renderLfChip(container, blocks) {
  const info = lfChipInfo(blocks);
  const fact = document.createElement('span');
  fact.className = 'tag-fact tag-capability';
  const label = document.createElement('span');
  label.className = 'tag-fact-label';
  label.textContent = 'Physical tag';
  const capability = document.createElement('span');
  capability.className = 'tag-fact-value';
  fact.append(label, capability);
  const details = document.createElement('details');
  details.className = 'tag-details';
  const summary = document.createElement('summary');
  summary.textContent = 'ⓘ';
  summary.setAttribute('aria-label', 'RFID chip details');
  const body = document.createElement('div');
  body.className = 'tag-detail-content';
  const note = document.createElement('p');

  if (info) {
    capability.textContent = info.programmingSupported
      ? 'T55x7 · Writing possible'
      : 'T55x7 · Writing unsupported';
    fact.classList.add(info.programmingSupported ? 'confirmed' : 'unsupported');
    const properties = document.createElement('dl');
    properties.className = 'lf-chip-properties';
    for (const [label, text] of [
      ['Radio', `${info.modulation} · RF/${info.clock}`],
      ['Password', info.passwordEnabled ? 'Enabled' : 'Disabled']
    ]) {
      const term = document.createElement('dt');
      term.textContent = label;
      const value = document.createElement('dd');
      value.textContent = text;
      properties.append(term, value);
    }
    body.append(properties);
    if (info.programmingSupported) {
      note.textContent = 'The target is checked again before writing.';
    } else if (info.passwordEnabled) {
      note.textContent = 'This writer does not support password-protected tags.';
    } else {
      note.textContent = 'This configuration is not supported by the T55xx writer.';
    }
  } else if (blocks === undefined) {
    capability.textContent = 'Physical tag not checked';
    note.textContent = 'The physical target is checked when you choose Program a tag.';
  } else {
    capability.textContent = 'Chip not identified · Writing unknown';
    note.textContent = 'Detection was inconclusive. Check the target again when programming.';
  }

  body.append(note);
  details.append(summary, body);
  container.replaceChildren(fact, details);
}
