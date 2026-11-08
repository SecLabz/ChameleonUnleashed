const PREFIX = 'chameleon-classic-keys-v1:';
const SECTORS = new Map([
  [20, 5],
  [64, 16],
  [128, 32],
  [256, 40]
]);
const bytes = (value, length) =>
  Array.isArray(value) &&
  value.length === length &&
  value.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 255);
const hex = (value) => value.map((byte) => byte.toString(16).padStart(2, '0')).join('');

function identity(card, geometry) {
  if (
    !card ||
    ![4, 7, 10].includes(card.uid?.length) ||
    !bytes(card.uid, card.uid.length) ||
    !bytes(card.atqa, 2) ||
    !Number.isInteger(card.sak) ||
    card.sak < 0 ||
    card.sak > 255 ||
    !SECTORS.has(geometry?.blocks) ||
    geometry.sectors?.length !== SECTORS.get(geometry.blocks)
  ) {
    return null;
  }
  return `${PREFIX}${hex(card.uid)}:${card.sak}:${hex(card.atqa)}:${geometry.blocks}`;
}

function decode(text, count) {
  if (!text || text.length > 12000) {
    return null;
  }
  const entry = JSON.parse(text);
  if (
    entry?.version !== 1 ||
    !Array.isArray(entry.keys) ||
    ![...SECTORS.values()].includes(entry.keys.length) ||
    (count !== undefined && entry.keys.length !== count) ||
    entry.keys.some(
      (keys) =>
        !keys ||
        ['key_a', 'key_b'].some((name) => keys[name] !== null && !bytes(keys[name], 6)) ||
        ![null, 'Trailer'].includes(keys.origin_b) ||
        (keys.origin_b === 'Trailer' && !keys.key_b)
    )
  ) {
    return null;
  }
  return entry.keys;
}

export function createClassicKeyCache(storage = () => globalThis.localStorage) {
  return {
    load(card, geometry) {
      const id = identity(card, geometry);
      try {
        return id ? decode(storage().getItem(id), geometry.sectors.length) || [] : [];
      } catch {
        return [];
      }
    },

    save(card, geometry, mapping) {
      const id = identity(card, geometry);
      if (!id || mapping?.length !== geometry.sectors.length) {
        return false;
      }
      const keys = mapping.map((entry) => ({
        key_a: bytes(entry?.key_a, 6) ? [...entry.key_a] : null,
        key_b: bytes(entry?.key_b, 6) ? [...entry.key_b] : null,
        origin_b: bytes(entry?.key_b, 6) && entry.origin_b === 'Trailer' ? 'Trailer' : null
      }));
      const text = keys.some(({ key_a, key_b }) => key_a || key_b)
        ? JSON.stringify({ version: 1, keys })
        : null;
      try {
        const store = storage();
        if (store.getItem(id) !== text) {
          if (text) {
            store.setItem(id, text);
          } else {
            store.removeItem(id);
          }
        }
        return true;
      } catch {
        return false;
      }
    },

    state() {
      try {
        const store = storage();
        let count = 0;
        for (let index = 0; index < store.length; index++) {
          const id = store.key(index);
          if (id?.startsWith(PREFIX)) {
            try {
              if (decode(store.getItem(id))?.some(({ key_a, key_b }) => key_a || key_b)) {
                count++;
              }
            } catch {
              // A damaged entry does not hide the remaining tags.
            }
          }
        }
        return { available: true, count };
      } catch {
        return { available: false, count: 0 };
      }
    },

    clear() {
      try {
        const store = storage();
        const ids = Array.from({ length: store.length }, (_, index) => store.key(index)).filter((id) =>
          id?.startsWith(PREFIX)
        );
        ids.forEach((id) => store.removeItem(id));
        return true;
      } catch {
        return false;
      }
    }
  };
}

export const classicKeyCache = createClassicKeyCache();
