import { DESFIRE_STATES, inspectDesfire } from '../documents/desfire.mjs';
import { hex } from '../device/transport.mjs';

function element(tag, className, text = '') {
  const node = document.createElement(tag);
  node.className = className;
  node.textContent = text;
  return node;
}

function property(list, name, value) {
  list.append(element('dt', '', name), element('dd', '', value));
}

function badge(state) {
  const node = element('span', 'desfire-state', DESFIRE_STATES[state]);
  node.dataset.state = state;
  return node;
}

function emptyState(title, description) {
  const node = element('div', 'desfire-empty');
  const icon = element('span', 'desfire-empty-icon');
  icon.setAttribute('aria-hidden', 'true');
  icon.innerHTML =
    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M8 13h8m-8 4h5"/></svg>';
  node.append(icon, element('h4', '', title), element('p', '', description));
  return node;
}

export function renderDesfire(tagDocument, host) {
  const details = inspectDesfire(tagDocument.capture);
  const panel = element('div', 'desfire-browser');
  host.replaceChildren(panel);

  if (details.full) {
    const overview = element('dl', 'desfire-overview');
    property(
      overview,
      'Applications',
      details.applications_state === 0 ? `${details.applications.length}` : '—'
    );
    property(
      overview,
      'Captured files',
      `${details.applications.reduce((count, app) => count + app.files.length, 0)}`
    );
    property(
      overview,
      'Free memory',
      details.free_memory === null ? 'Unavailable' : `${details.free_memory} B`
    );
    panel.append(overview);
  }

  if (!details.full) {
    panel.append(
      emptyState(
        'Applications not read yet',
        'Only the card identity is available. Read the card to discover public applications and files.'
      )
    );
  } else if (details.applications_state !== 0) {
    panel.append(
      emptyState(
        `Application directory · ${DESFIRE_STATES[details.applications_state]}`,
        'No application list is available in this capture.'
      )
    );
  } else if (!details.applications.length) {
    panel.append(
      emptyState(
        'No applications found',
        'The public directory is empty. There are no application files to display.'
      )
    );
  }

  for (const application of details.applications) {
    const section = document.createElement('details');
    section.className = 'desfire-application';
    section.open = true;

    const title = document.createElement('summary');
    title.className = 'desfire-row';
    const identity = element('span', 'desfire-label');
    identity.append(
      element('span', 'desfire-caption', 'Application'),
      element('code', '', hex(application.aid))
    );
    title.append(identity);
    if (application.files_state === 0) {
      title.append(element('span', 'desfire-size', `${application.files.length} files`));
    }
    title.append(badge(application.files_state));
    section.append(title);

    const files = element('div', 'desfire-files');
    if (!application.files.length) {
      const message =
        application.files_state === 0
          ? 'No files in this application.'
          : `File directory · ${DESFIRE_STATES[application.files_state]}`;
      files.append(element('p', 'desfire-note', message));
    }
    for (const file of application.files) {
      const entry = document.createElement('details');
      entry.className = 'desfire-file';
      const label = document.createElement('summary');
      label.className = 'desfire-row';
      const type =
        ['Standard data', 'Backup data', 'Value', 'Linear records', 'Cyclic records', 'Transaction MAC'][
          file.settings[0]
        ] || 'Unknown type';
      const identity = element('span', 'desfire-label');
      identity.append(
        element('span', 'desfire-file-name', `File ${file.id}`),
        element('span', 'desfire-caption', type)
      );
      label.append(
        identity,
        element('span', 'desfire-size', `${file.data.length} B captured`),
        badge(file.data_state)
      );
      entry.append(label);

      const properties = element('dl', 'desfire-properties');
      property(properties, 'Settings', DESFIRE_STATES[file.settings_state]);
      if (file.settings.length) {
        property(properties, 'Communication', hex(file.settings.slice(1, 2)));
        property(properties, 'Access', hex(file.settings.slice(2, 4)));
      }
      entry.append(properties);

      if (file.data.length) {
        const pre = element('pre', 'desfire-data');
        pre.tabIndex = 0;
        pre.setAttribute('aria-label', `File ${file.id} captured data in hexadecimal and ASCII`);
        pre.textContent = Array.from({ length: Math.ceil(file.data.length / 16) }, (_, rowIndex) => {
          const row = file.data.slice(rowIndex * 16, rowIndex * 16 + 16);
          const address = (rowIndex * 16).toString(16).padStart(4, '0');
          const text = row
            .map((byte) => (byte >= 32 && byte < 127 ? String.fromCharCode(byte) : '.'))
            .join('');
          return `${address}  ${hex(row).padEnd(32)}  ${text}`;
        }).join('\n');
        entry.append(pre);
      } else if (file.data_state !== 0) {
        entry.append(element('p', 'desfire-note', 'No public data captured for this file.'));
      }
      files.append(entry);
    }
    section.append(files);
    panel.append(section);
  }

  const card = document.createElement('details');
  card.className = 'desfire-card-details';
  card.append(element('summary', '', 'Card details'));
  const properties = element('dl', 'desfire-properties');
  property(properties, 'ATS', hex(details.card.ats));
  if (details.full) {
    property(properties, 'PICC settings', DESFIRE_STATES[details.keys.status]);
  }
  card.append(properties);
  panel.append(card);
}
