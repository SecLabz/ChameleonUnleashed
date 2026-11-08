import { bytesValid } from './slot-image.mjs';
import { classicObservationMatches } from './classic-magic.mjs';
import { hex } from '../device/transport.mjs';

const validKey = (key) => key?.length === 6 && bytesValid(key);

export function classicDocumentKeys(document) {
  const observation =
    [8, 9, 16, 24].includes(document?.card?.sak & ~0x20) &&
    classicObservationMatches(document?.classicObservation, document?.card)
      ? document.classicObservation
      : null;
  const observedKey = (keyB) =>
    observation?.credentials?.find((entry) => entry.keyB === keyB && validKey(entry.key))?.key || null;
  const observedKeys = { key_a: observedKey(false), key_b: observedKey(true) };
  if (document?.kind === 'classic') {
    return (document.geometry?.sectors || []).map((sector, index) => {
      const trailer = document.memory?.[sector.trailer];
      const recovered = document.recoveredKeys?.[index];
      const keyAt = (start, name) => {
        // RF masks unknown keys with zeros; only recovery or an explicit edit
        // can establish a key when this document has recovery metadata.
        if (recovered && !validKey(recovered[name])) {
          return null;
        }
        const bytes = trailer?.slice(start, start + 6);
        // Edited/imported memory takes precedence. A partial trailer must not
        // hide a recovered key that is retained separately from memory.
        const key = validKey(bytes)
          ? bytes
          : (recovered?.[name] ?? (index === 0 ? observedKeys[name] : null));
        return validKey(key) ? key : null;
      };
      return { key_a: keyAt(0, 'key_a'), key_b: keyAt(10, 'key_b') };
    });
  }

  if (document?.kind === 'identity' && observation) {
    return [observedKeys];
  }

  return [];
}

export function classicDictionaryKeys(document) {
  return [
    ...new Set(
      classicDocumentKeys(document)
        .flatMap(({ key_a, key_b }) => [key_a, key_b])
        .filter(Boolean)
        .map(hex)
    )
  ];
}
