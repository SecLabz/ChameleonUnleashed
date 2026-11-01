import { hex } from '../device/transport.mjs';
import { editLf, encodeLfFields, HID_FORMATS, lfFields, lfSchema, LF_TYPES } from '../documents/lf.mjs';
import { typeName } from '../documents/slot-image.mjs';
import { $ } from './device-view.mjs';

export function lfEditor(state, { onChange, onFormat }) {
  for (const type of LF_TYPES) {
    $('lf-format').add(new Option(typeName(type), String(type)));
  }

  function updateRecord() {
    const data = state.document?.memory[0];
    $('lf-record').hidden = !data || ![104, 200, 201, 310].includes(state.document.type);
    $('lf-record-value').textContent = data ? hex(data) : '';
  }

  function showError(error) {
    $('lf-error').hidden = !error;
    $('lf-error').textContent = error?.message || '';
    for (const input of $('lf-fields').querySelectorAll('input')) {
      input.setAttribute('aria-invalid', String(error?.field === input.name));
      const ids = [`${input.id}-help`];
      if (error?.field === input.name) {
        ids.push('lf-error');
      }
      input.setAttribute('aria-describedby', ids.join(' '));
    }
  }

  function fields() {
    const values = {};
    for (const input of $('lf-fields').querySelectorAll('input, select')) {
      values[input.name] = input.value;
    }
    return values;
  }

  function renderFields(values) {
    const doc = state.document;
    $('lf-fields').replaceChildren();
    if (doc.type === 200) {
      const label = document.createElement('label');
      label.className = 'field lf-wide';
      label.textContent = 'HID format';
      const select = document.createElement('select');
      select.name = 'format';
      select.id = 'lf-hid-format';
      for (const [name, code] of HID_FORMATS) {
        select.add(new Option(name, String(code)));
      }
      if (!HID_FORMATS.some((entry) => String(entry[1]) === values.format)) {
        select.add(new Option(`Unknown format (${values.format})`, values.format));
      }
      select.value = values.format;
      label.append(select);
      $('lf-fields').append(label);
    }

    for (const field of lfSchema(doc.type, values)) {
      const label = document.createElement('label');
      label.className = 'field';
      label.textContent = field.label;
      if (field.limit === undefined) {
        label.classList.add('lf-wide');
      }
      const input = document.createElement('input');
      input.name = field.key;
      input.id = `lf-${field.key}`;
      input.value = values[field.key] ?? '';
      input.autocomplete = 'off';
      input.spellcheck = false;
      if (field.limit !== undefined) {
        input.inputMode = 'numeric';
      } else {
        input.className = 'lf-id';
        input.autocapitalize = doc.type === 150 ? 'off' : 'characters';
      }
      const hint = document.createElement('small');
      hint.className = 'memory-hint';
      hint.id = `${input.id}-help`;
      hint.textContent = field.help;
      input.setAttribute('aria-describedby', hint.id);
      label.append(input, hint);
      $('lf-fields').append(label);
    }
  }

  function render() {
    const doc = state.document;
    $('lf-format').value = String(doc.type);
    const values = lfFields(doc);
    renderFields(values);
    let error;
    if (doc.lf?.input) {
      try {
        encodeLfFields(doc.type, values);
      } catch (failure) {
        error = failure;
      }
    }
    showError(error);
    const hints = [];
    if (doc.type === 310) {
      hints.push('The fixed IDTK prefix is added automatically.');
    }
    if (doc.lf?.clock && !doc.lf?.chip) {
      hints.push(`Recorded radio clock: RF/${doc.lf.clock}.`);
    }
    hints.push('Radio settings follow the format. Programming requires a T55x7 tag.');
    $('lf-hint').textContent = hints.join(' ');
    updateRecord();
  }

  $('lf-format').onchange = async () => {
    const type = Number($('lf-format').value);
    await onFormat(type);
    $('lf-format').value = String(state.document.type);
  };

  $('lf-fields').oninput = (event) => {
    if (event.target.tagName !== 'INPUT') {
      return;
    }
    const error = editLf(state.document, fields());
    showError(error);
    updateRecord();
    onChange();
  };

  $('lf-fields').onchange = (event) => {
    if (event.target.tagName === 'SELECT') {
      const values = fields();
      values.issue = '0';
      values.oem = '900';
      renderFields(values);
      const error = editLf(state.document, fields());
      showError(error);
      updateRecord();
      onChange();
    } else if (
      event.target.classList.contains('lf-id') &&
      state.document.type !== 150 &&
      state.document.memory[0]
    ) {
      // Input already committed the bytes. Avoid rebuilding action labels during blur:
      // WebKit can otherwise lose the click that opened a toolbar menu.
      event.target.value = event.target.value.replace(/\s/g, '').toUpperCase();
    }
  };

  return { render };
}
