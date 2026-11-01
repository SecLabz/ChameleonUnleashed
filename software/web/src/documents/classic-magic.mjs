const GEN1A = 1;
const GEN2 = 1 << 2;
const UNFUSED = 1 << 3;
const SIGNATURES = [
  [GEN1A, 'Gen1A'],
  [1 << 1, 'Gen1B-compatible'],
  [GEN2, 'Gen2 / CUID'],
  [UNFUSED, 'Unfused UID signature'],
  [1 << 4, 'Super Gen1'],
  [1 << 5, 'Super Gen2'],
  [1 << 7, 'Gen3'],
  [1 << 8, 'Gen4 GTU / UMC (default password)'],
  [1 << 9, 'GDM / USCUID (magic auth)'],
  [1 << 10, 'QL88'],
  [1 << 11, 'GDM / USCUID (20 wakeup)'],
  [1 << 12, 'GDM / USCUID (40 wakeup)'],
  [1 << 13, 'GDM (ZUID-compatible)']
];
export const GEN2_INCONCLUSIVE = 5;
const GEN2_EVIDENCE = [
  'Not checked with a sector-0 key',
  'Authentication refused or incomplete',
  'Block-0 WRITE not acknowledged; no data sent',
  'Block-0 WRITE acknowledged; no data sent',
  'Known ATS signature detected',
  'WRITE reply inconclusive; no data sent'
];

export function classicObservationMatches(observation, card) {
  return (
    !!observation &&
    !!card &&
    observation.card.sak === card.sak &&
    observation.card.uid.join() === card.uid.join() &&
    observation.card.atqa.join() === card.atqa.join()
  );
}

export function decodeClassicMagic(bytes) {
  const flags = (bytes[0] << 8) | bytes[1];
  const gen2 = bytes[2];
  if (
    bytes.length !== 3 ||
    gen2 > 5 ||
    flags & ~0x3fbf ||
    !!(flags & GEN2) !== [3, 4].includes(gen2) ||
    (flags & 3) === 3 ||
    (flags & 0x30) === 0x30 ||
    (flags & 0x3000) === 0x3000
  ) {
    throw new Error('Invalid magic signatures or Gen2 evidence');
  }
  return { flags, gen2 };
}

export function magicGen1aDetected(magic) {
  return !!(magic.flags & GEN1A);
}

export function magicCloneSupported(magic) {
  // Reject other generations and known unfused signatures. Some OTP variants
  // remain indistinguishable from rewritable targets without writing.
  const supported = GEN1A | GEN2;
  return !!(magic.flags & supported) && !(magic.flags & ~supported);
}

export function magicGen2Confirmed(magic) {
  return !!(magic.flags & GEN2);
}

export function magicGenerationDetected(magic) {
  return !!(magic.flags & ~UNFUSED);
}

export function classicMagicSignatures(magic) {
  return SIGNATURES.filter(([flag]) => magic.flags & flag).map(([, name]) => name);
}

export function classicMagicEvidence(magic) {
  return GEN2_EVIDENCE[magic.gen2];
}

export function maintenanceMethod(magic) {
  if (!magic || magic.flags & ~7) {
    return null;
  }
  if (magic.flags & 1) {
    return 1;
  }
  if (magic.flags & 2) {
    return 2;
  }
  return magic.flags & 4 ? 3 : null;
}
