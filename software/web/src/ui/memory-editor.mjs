// Unified memory editor for every tag family: one textarea, an annotated gutter,
// a coloured overlay and a read-only ASCII column.
import { ST25TB } from '../device/st25tb-protocol.mjs';

const hexByte = (byte) => byte.toString(16).padStart(2, '0').toUpperCase();

const rowNumber = (number) => String(number).padStart(3, '0');

const escapeHtml = (text) =>
  text.replace(/[&<>]/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[char]);
const TAG_TONES = {
  OTP: 'lock',
  sysOTP: 'config',
  UID: 'uid',
  Lock: 'lock',
  CC: 'cc',
  Config: 'config',
  PWD: 'config',
  PACK: 'config',
  Auth: 'config',
  Key: 'config',
  Counter: 'counter'
};
const LOCK_ICON = `<svg viewBox="0 0 12 12" fill="none" stroke="currentColor"
  stroke-width="1.4" aria-hidden="true"><rect x="2.5" y="5.5" width="7" height="5" rx="1"/>
  <path d="M4 5.5V4a2 2 0 0 1 4 0v1.5"/></svg>`;
export const EDITOR_LEGENDS = {
  classic: [
    ['manufacturer', 'UID / manufacturer'],
    ['key-a', 'Key A'],
    ['access', 'Access + GPB'],
    ['key-b', 'Key B'],
    ['unknown', '?? unread']
  ],
  type2: [
    ['uid', 'UID'],
    ['lock', 'Lock'],
    ['cc', 'CC'],
    ['config', 'Config']
  ],
  st25tb: [
    ['counter', 'Counter'],
    ['config', 'Config']
  ],
  nfcv: [['unknown', '?? unread']]
};

// Only pages with a role get a label; plain user memory stays unannotated.
function type2Tags(document) {
  const count = document.memory.length;

  const tags = {
    0: 'UID',
    1: 'UID',
    2: 'Lock',
    3: document.type >= 1103 && document.type <= 1106 ? 'OTP' : 'CC'
  };

  if (document.geometry?.profile === 'ultralight_c' || document.type === 1104) {
    Object.assign(tags, {
      40: 'Lock',
      41: 'Counter',
      42: 'Auth',
      43: 'Auth',
      44: 'Key',
      45: 'Key',
      46: 'Key',
      47: 'Key'
    });
  } else if (count > 16) {
    const end = [...(count > 20 ? ['Lock'] : []), 'Config', 'Config', 'PWD', 'PACK'];
    end.forEach((tag, i) => {
      tags[count - end.length + i] = tag;
    });
  }

  return tags;
}

// One row per line of the textarea: gutter labels, byte grouping and parsing rules.
function editorLayout(document) {
  if (document.kind === 'st25tb') {
    const blocks = [...document.memory, document.card.system ?? document.capture?.system ?? null];
    const rows = blocks.map((block, index) => {
      const systemBlock = index === ST25TB.BLOCKS;
      let tag = '';
      if (systemBlock) {
        tag = 'sysOTP';
      } else if (document.type === ST25TB.TYPE && ST25TB.COUNTER_BLOCKS.includes(index)) {
        tag = 'Counter';
      }

      return {
        block,
        groups: [4],
        divider: systemBlock,
        number: index < ST25TB.BLOCKS ? rowNumber(index) : String(ST25TB.SYSTEM_BLOCK),
        tag
      };
    });

    return {
      unit: 4,
      noun: 'block',
      msbFirst: true,
      hint: 'One word per line · most significant byte first · ???????? for unread',
      rows
    };
  }

  if (document.kind === 'classic') {
    const sectors = document.geometry.sectors;
    return {
      unit: 16,
      noun: 'block',
      partial: true,
      sectors: true,
      hint: 'One block per line · groups of 4 bytes · ?? marks unread bytes',
      rows: document.memory.map((block, i) => {
        const index = sectors.findIndex((sector) => i >= sector.first && i < sector.first + sector.count);
        const sector = sectors[index];
        const row = {
          block,
          number: rowNumber(i),
          groups: [4, 4, 4, 4],
          sector: i === sector.first ? `S${index}` : '',
          divider: i === sector.first && i > 0,
          tag: i === 0 ? 'UID' : ''
        };
        if (i === 0) {
          row.tones = Array(4).fill('manufacturer');
        }
        if (i === sector.trailer) {
          Object.assign(row, {
            groups: [6, 4, 6],
            tones: ['key-a', 'access', 'key-b'],
            keys: [false, false]
          });
        }
        return row;
      })
    };
  }

  const tags = document.kind === 'type2' ? type2Tags(document) : {};
  const noun = document.kind === 'type2' ? 'page' : 'block';
  return {
    unit: document.unitBytes,
    noun,
    hint: `One ${noun} per line · ?? marks unread bytes`,
    rows: document.memory.map((block, i) => ({
      block,
      number: rowNumber(i),
      groups: [document.unitBytes],
      tag: tags[i] || '',
      locked: document.locks?.[i]
    }))
  };
}

function formatRow(row, layout) {
  let bytes = row.block || Array(layout.unit).fill(null);

  if (layout.msbFirst) {
    bytes = [...bytes].reverse();
  }
  const cells = bytes.map((byte) => (byte === null ? '??' : hexByte(byte)));

  const groups = [];
  let offset = 0;
  for (const size of row.groups) {
    groups.push(cells.slice(offset, offset + size).join(''));
    offset += size;
  }
  return groups.join(' ');
}

// Spaces are ignored; ?? keeps a byte unknown. Returns bytes in memory order, or null for an unread row.
function parseRow(text, row, layout) {
  const value = text.replace(/\s/g, '').toUpperCase();
  if (!new RegExp(`^(?:[0-9A-F]{2}|\\?\\?){${layout.unit}}$`).test(value)) {
    throw new Error(`enter ${layout.unit} bytes in hexadecimal`);
  }

  const cells = value.match(/../g).map((pair) => (pair === '??' ? null : parseInt(pair, 16)));
  if (cells.every((cell) => cell === null) && !(layout.msbFirst && row.block)) {
    return null;
  }
  if (!layout.partial && cells.includes(null) && !layout.msbFirst) {
    throw new Error('enter complete hex bytes, or ?? for the entire unread row');
  }

  if (layout.msbFirst) {
    if (cells.includes(null)) {
      throw new Error('enter eight hex digits');
    }
    if (!row.block) {
      throw new Error('unread data cannot be written');
    }
    return cells.reverse();
  }

  return cells;
}

// Colours follow the digit position, so the overlay stays right while the user types.
function paintLine(text, row, bad) {
  const groupEnds = row?.tones
    ? row.groups.reduce((list, size) => [...list, (list.at(-1) || 0) + size * 2], [])
    : null;
  let digit = 0;
  let output = '';
  let textRun = '';
  let tone = '';

  const flush = () => {
    if (textRun) {
      output += tone ? `<span class="t-${tone}">${escapeHtml(textRun)}</span>` : escapeHtml(textRun);
    }
    textRun = '';
  };

  for (const char of text) {
    let nextTone = '';
    if (char === '?') {
      nextTone = 'unknown';
    } else if (groupEnds && /[0-9a-f]/i.test(char)) {
      nextTone = row.tones[groupEnds.findIndex((end) => digit < end)] || '';
    }
    if (!/\s/.test(char)) {
      digit++;
    }
    if (nextTone !== tone) {
      flush();
      tone = nextTone;
    }
    textRun += char;
  }

  flush();

  const dividerClass = row?.divider ? ' divider' : '';
  const invalidClass = bad ? ' bad' : '';
  return `<span class="hx-line${dividerClass}${invalidClass}">${output || ' '}</span>`;
}

function gutterRow(row, bad, columns) {
  let tag = row.tag;
  if (row.keys) {
    tag = ['A', 'B']
      .map((name, index) => {
        const className = row.keys[index] ? `t-key-${name.toLowerCase()}` : 'k-missing';
        const status = row.keys[index] ? '✓' : '?';
        return `<b class="${className}">${name}${status}</b>`;
      })
      .join(' ');
  }

  let lock = '';
  if (row.locked === true) {
    lock = LOCK_ICON;
  } else if (row.locked === null) {
    lock = '?';
  }
  const tone = row.tag ? ` t-${TAG_TONES[row.tag]}` : '';
  const sector = columns.sectors ? `<span class="hx-sector">${row.sector}</span>` : '';
  const label = columns.tags ? `<span class="hx-tag${tone}">${tag}</span>` : '';
  const lockCell = columns.locks ? `<span class="hx-lock">${lock}</span>` : '';
  const dividerClass = row.divider ? ' divider' : '';
  const invalidClass = bad ? ' bad' : '';

  return (
    `<div class="hx-row${dividerClass}${invalidClass}">` +
    `${sector}<span class="hx-num">${row.number}</span>${label}${lockCell}</div>`
  );
}

// Zero bytes stay blank so text such as an NDEF URI stands out from empty memory.
function ascii(bytes) {
  if (!bytes) {
    return '';
  }

  return bytes
    .map((byte) => {
      if (byte === null) {
        return '?';
      }
      if (!byte) {
        return ' ';
      }
      return byte >= 32 && byte < 127 ? String.fromCharCode(byte) : '·';
    })
    .join('');
}

// onInput(parsed) receives one entry per row (bytes or null), or null while a line is invalid.
export function memoryEditor(document, { busy = false, onInput }) {
  const layout = editorLayout(document);
  const rows = layout.rows;
  const columns = {
    sectors: !!layout.sectors,
    tags: rows.some((row) => row.tag || row.keys),
    locks: !!document.locks
  };

  const make = (tag, className) =>
    Object.assign(globalThis.document.createElement(tag), className ? { className } : {});

  const root = make('div', `hx${busy ? ' busy' : ''}`);
  root.classList.toggle('hx-classic', document.kind === 'classic');
  const gutter = make('div', 'hx-gutter');
  const code = make('div', 'hx-code');
  const overlay = make('pre');
  const editor = make('textarea');
  const asciiColumn = make('pre', 'hx-ascii');
  const message = make('p', 'hx-message');

  gutter.setAttribute('aria-hidden', 'true');
  overlay.setAttribute('aria-hidden', 'true');
  asciiColumn.setAttribute('aria-hidden', 'true');

  const tagWidth = Math.max(0, ...rows.map((row) => (row.keys ? 5 : row.tag.length)));
  if (tagWidth) {
    gutter.style.setProperty('--tag-w', `${tagWidth}ch`);
  }

  Object.assign(editor, {
    spellcheck: false,
    wrap: 'off',
    readOnly: busy,
    autocomplete: 'off',
    value: rows.map((row) => formatRow(row, layout)).join('\n')
  });
  editor.setAttribute('autocapitalize', 'characters');
  editor.setAttribute('aria-label', `Memory, one ${layout.noun} per line in hexadecimal`);

  message.id = `hx-message-${Math.random().toString(36).slice(2, 8)}`;
  editor.setAttribute('aria-describedby', message.id);
  message.setAttribute('aria-live', 'polite');

  const paint = () => {
    const lines = editor.value.replace(/\r/g, '').split('\n');
    const parsed = [];
    const errors = [];
    while (lines.length > rows.length && !lines.at(-1).trim()) {
      lines.pop();
    }

    lines.forEach((line, i) => {
      if (!rows[i]) {
        errors.push([i, `no ${layout.noun} on this line`]);
        return;
      }
      try {
        parsed[i] = parseRow(line, rows[i], layout);
      } catch (error) {
        errors.push([i, error.message]);
      }
    });
    if (lines.length < rows.length) {
      errors.push([lines.length, `${rows.length - lines.length} line(s) missing`]);
    }

    const bad = new Set(errors.map(([i]) => i));
    rows.forEach((row, i) => {
      if (row.keys) {
        row.keys = [0, 10].map(
          (start) => !!parsed[i]?.slice(start, start + 6).every((byte) => byte !== null)
        );
      }
    });

    overlay.innerHTML = lines.map((line, i) => paintLine(line, rows[i], bad.has(i))).join('');
    gutter.innerHTML = rows.map((row, i) => gutterRow(row, bad.has(i), columns)).join('');
    asciiColumn.innerHTML = lines
      .map(
        (_, i) =>
          `<span class="hx-line${rows[i]?.divider ? ' divider' : ''}">${escapeHtml(ascii(parsed[i])) || ' '}</span>`
      )
      .join('');

    const [first] = errors;
    message.classList.toggle('error', !!first);
    message.textContent = layout.hint;
    if (first) {
      const [index, detail] = first;
      const row = rows[index];
      const location = row ? ` · ${layout.noun} ${row.number}` : '';
      const remaining = errors.length > 1 ? ` · ${errors.length - 1} more` : '';
      message.textContent = `Line ${index + 1}${location}: ${detail}${remaining}`;
    }

    editor.setAttribute('aria-invalid', String(!!first));
    return first ? null : parsed;
  };

  paint();

  editor.oninput = () => {
    const parsed = paint();
    editor.scrollTop = 0;
    editor.scrollLeft = 0;
    onInput(parsed);
  };

  code.append(overlay, editor);
  root.append(gutter, code, asciiColumn);
  return [root, message];
}
