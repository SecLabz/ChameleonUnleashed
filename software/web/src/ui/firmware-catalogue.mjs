import { sha256 } from '../runtime/sha256.mjs';
import { MAXIMUM_SIZE, MINIMUM_ZIP_SIZE, RECOVERY_LAYOUT } from './nordic-package.mjs';
const CATALOGUE_FORMAT = 1;
const RELEASES_PER_SOURCE = 5;
const SOURCE_IDS = ['unleashed', 'official'];
export function versionLabel(value) {
  return typeof value === 'string' ? value : `${value >>> 24}.${(value >>> 16) & 255}.${value & 65535}`;
}

const updateBase = (base, development) =>
  new URL(development ? 'updates/dev/' : 'updates/', new URL(base, globalThis.location?.href));
function validAsset(asset) {
  return (
    asset &&
    Number.isInteger(asset.size) &&
    asset.size >= MINIMUM_ZIP_SIZE &&
    asset.size <= MAXIMUM_SIZE &&
    /^[a-f0-9]{64}$/.test(asset.sha256) &&
    new RegExp(`^(?:files/)?${asset.sha256}\\.zip$`).test(asset.path)
  );
}

export async function loadCatalogue(signal, development = false, base) {
  const response = await fetch(new URL('catalog.json', updateBase(base, development)), {
    signal,
    cache: 'no-store'
  });
  if (!response.ok) {
    throw new Error('Release list unavailable. You can still choose a local Nordic ZIP.');
  }
  const catalogue = await response.json();
  if (
    !catalogue ||
    catalogue.format !== CATALOGUE_FORMAT ||
    !Array.isArray(catalogue.sources) ||
    catalogue.sources.length > SOURCE_IDS.length ||
    new Set(catalogue.sources.map((source) => source?.id)).size !== catalogue.sources.length ||
    catalogue.sources.some(
      (source) =>
        !source ||
        !SOURCE_IDS.includes(source.id) ||
        typeof source.label !== 'string' ||
        !Array.isArray(source.releases) ||
        source.releases.length > RELEASES_PER_SOURCE ||
        new Set(source.releases.map((release) => release?.version)).size !== source.releases.length ||
        source.releases.some(
          (release) =>
            !release ||
            typeof release.version !== 'string' ||
            !release.version.length ||
            ![0, RECOVERY_LAYOUT].includes(release.layout) ||
            !validAsset(release.packages?.[release.layout === RECOVERY_LAYOUT ? 'application' : 'full'])
        )
    )
  ) {
    throw new Error('Invalid release catalogue.');
  }
  return catalogue;
}
export async function downloadFirmware(asset, signal, onProgress = () => {}, development = false, base) {
  if (!validAsset(asset)) {
    throw new Error('Invalid firmware download metadata.');
  }
  const file = await fetch(new URL(asset.path, updateBase(base, development)), { signal, cache: 'no-store' });
  if (!file.ok || !file.body) {
    throw new Error('Firmware download failed. Choose a local Nordic ZIP to continue.');
  }
  const bytes = new Uint8Array(asset.size);
  const reader = file.body.getReader();
  let offset = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      if (value.length > bytes.length - offset) {
        throw new Error('The firmware download is larger than expected.');
      }
      bytes.set(value, offset);
      offset += value.length;
      onProgress(offset / bytes.length, 'Downloading firmware…');
    }
  } finally {
    await reader.cancel().catch(() => {});
  }
  if (offset !== bytes.length || (await sha256(bytes, signal)) !== asset.sha256) {
    throw new Error('Firmware download verification failed. Try again.');
  }
  return bytes;
}
