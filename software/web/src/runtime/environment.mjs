// Hosted packages add these markers. The USB-served app keeps its HTTP backend
// and relies on the device to serve the interface, even without Internet.
export const HOSTED = !!globalThis.document?.querySelector('meta[name="chameleon-hosted"]');
export const SHELL_VERSION = globalThis.document
  ?.querySelector('meta[name="chameleon-shell"]')
  ?.getAttribute('content');
/** @param {string} path */
export const appUrl = (path) => (globalThis.document ? new URL(path, document.baseURI).href : '/' + path);
