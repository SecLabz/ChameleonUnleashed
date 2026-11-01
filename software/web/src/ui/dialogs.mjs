export function ask({
  title,
  message,
  warning,
  initial,
  confirmLabel = 'Continue',
  signal,
  inputType = 'text',
  validateInput
}) {
  if (signal?.aborted) {
    return Promise.resolve(initial === undefined ? false : null);
  }
  const dialog = document.createElement('dialog');
  dialog.className = 'app-dialog';
  const form = document.createElement('form');
  const heading = document.createElement('h2');
  heading.textContent = title;
  heading.id = 'question-title';
  dialog.setAttribute('aria-labelledby', heading.id);

  const description = document.createElement('p');
  description.textContent = message;
  description.id = 'question-description';
  dialog.setAttribute('aria-describedby', description.id);
  form.append(heading, description);

  if (warning) {
    const caution = document.createElement('p');
    caution.className = 'dialog-warning';
    caution.textContent = warning;
    caution.id = 'question-warning';
    dialog.setAttribute('aria-describedby', `${description.id} ${caution.id}`);
    form.append(caution);
  }

  let input;
  if (initial !== undefined) {
    input = document.createElement('input');
    input.value = initial;
    input.type = inputType;
    input.className = 'dialog-input';
    input.setAttribute('aria-label', title);
    input.autocomplete = 'off';
    form.append(input);
  }

  const actions = document.createElement('div');
  actions.className = 'dialog-actions';

  const cancel = document.createElement('button');
  cancel.type = 'button';
  cancel.className = 'secondary';
  cancel.textContent = 'Cancel';
  cancel.onclick = () => dialog.close();

  const confirm = document.createElement('button');
  confirm.type = 'submit';
  confirm.textContent = confirmLabel;
  confirm.className = 'primary-action';

  if (input && validateInput) {
    const feedback = document.createElement('p');
    feedback.className = 'memory-hint';
    feedback.setAttribute('aria-live', 'polite');
    input.oninput = () => {
      const message = validateInput(input.value);
      feedback.textContent = message;
      confirm.disabled = !!message;
    };
    input.oninput();
    form.append(feedback);
  }

  actions.append(cancel, confirm);
  form.append(actions);
  dialog.append(form);

  let result = initial === undefined ? false : null;
  form.onsubmit = (event) => {
    event.preventDefault();
    if (confirm.disabled) {
      return;
    }
    result = input ? input.value : true;
    dialog.close();
  };

  document.body.append(dialog);
  return new Promise((resolve) => {
    const abort = () => dialog.close();
    signal?.addEventListener('abort', abort, { once: true });
    dialog.addEventListener(
      'close',
      () => {
        signal?.removeEventListener('abort', abort);
        dialog.remove();
        resolve(result);
      },
      { once: true }
    );

    dialog.showModal();
    if (input) {
      input.focus();
      input.select();
    } else {
      cancel.focus();
    }
  });
}

export function download(text, name, type = 'text/plain') {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  document.body.append(link);
  link.click();
  link.remove();
  // Browsers can consume the blob asynchronously after the click returns.
  setTimeout(() => URL.revokeObjectURL(url), 60000);
}
