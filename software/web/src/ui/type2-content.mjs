import { amiiboInfo, applyNdef, inspectNdef } from '../documents/type2-content.mjs';

export function type2ContentView(host, onChange) {
  let busy = false;
  function setBusy(value) {
    busy = value;
    for (const input of host.querySelectorAll('input, textarea, select, button')) {
      input.disabled = busy || (input.type === 'submit' && !!host.querySelector('[aria-invalid="true"]'));
    }
  }

  function render(doc, working) {
    busy = working;
    host.replaceChildren();
    host.hidden = doc?.kind !== 'type2' || !!amiiboInfo(doc);
    if (host.hidden) {
      return;
    }
    const title = document.createElement('h3');
    const detail = document.createElement('p');
    detail.className = 'memory-hint';
    host.append(title, detail);
    title.textContent = 'Content';
    const info = inspectNdef(doc);
    detail.textContent = info.message || `${info.end - info.start} bytes available`;
    if (!info.editable) {
      for (const record of info.records) {
        const line = document.createElement('p');
        line.className = 'ndef-preview';
        if (record.kind === 'raw') {
          line.textContent = `Other record · ${record.bytes} bytes`;
        } else {
          const label = record.kind === 'uri' ? 'URL' : 'Text';
          line.textContent = `${label} · ${record.value}`;
        }
        host.append(line);
      }
      return;
    }
    const form = document.createElement('form');
    const typeLabel = document.createElement('label');
    typeLabel.className = 'field';
    typeLabel.textContent = 'NDEF type';
    const type = document.createElement('select');
    type.setAttribute('aria-label', 'NDEF type');
    for (const [value, label] of [
      ['text', 'Text'],
      ['uri', 'URL']
    ]) {
      const option = document.createElement('option');
      option.value = value;
      option.textContent = label;
      type.append(option);
    }
    type.value = info.records[0]?.kind || 'text';
    typeLabel.append(type);
    const label = document.createElement('label');
    label.className = 'field';
    label.textContent = 'Content';
    const value = document.createElement('textarea');
    value.setAttribute('aria-label', 'NDEF content');
    value.rows = 3;
    value.value = info.records[0]?.value || '';
    value.spellcheck = false;
    label.append(value);
    const feedback = document.createElement('p');
    feedback.className = 'memory-hint';
    feedback.setAttribute('role', 'status');
    const apply = document.createElement('button');
    apply.className = 'btn';
    apply.type = 'submit';
    apply.textContent = 'Apply to document';
    const validate = () => {
      try {
        applyNdef(doc, type.value, value.value);
        feedback.textContent = 'Edits stay in this document until you write, send or export it.';
        value.removeAttribute('aria-invalid');
        apply.disabled = busy;
      } catch (error) {
        feedback.textContent = error.message;
        value.setAttribute('aria-invalid', 'true');
        apply.disabled = true;
      }
    };
    value.oninput = validate;
    type.onchange = validate;
    form.onsubmit = (event) => {
      event.preventDefault();
      if (busy || apply.disabled) {
        return;
      }
      try {
        doc.memory = applyNdef(doc, type.value, value.value);
        onChange();
        host.querySelector('button')?.focus();
      } catch (error) {
        feedback.textContent = error.message;
      }
    };
    form.append(typeLabel, label, feedback, apply);
    host.append(form);
    setBusy(busy);
    validate();
  }
  return { render, setBusy };
}
