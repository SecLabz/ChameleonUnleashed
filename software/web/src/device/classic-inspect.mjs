import { ESSENTIALS } from '../documents/classic-keys.mjs';
import { decodeClassicMagic } from '../documents/classic-magic.mjs';
import { COMMAND } from './commands.mjs';
import { decodeClassicAts } from './classic-model.mjs';

export async function inspectClassic(device, card, onProgress = () => {}, { modelEvidence = false } = {}) {
  const observation = {
    card: { uid: [...card.uid], atqa: [...card.atqa], sak: card.sak },
    credentials: []
  };
  try {
    onProgress('Checking Classic capabilities…');
    const payload = [
      card.uid.length,
      ...card.uid,
      ...Array(10 - card.uid.length).fill(0),
      ...[...card.atqa].reverse(),
      card.sak,
      ESSENTIALS.length,
      ...ESSENTIALS.flat()
    ];
    let reply;
    try {
      reply = await device.command(COMMAND.CLASSIC_INSPECT, modelEvidence ? [...payload, 1] : payload);
    } catch (error) {
      if (!modelEvidence || error.status !== 0x60 || device.signal?.aborted) {
        throw error;
      }
      // Older firmware rejects the optional request before running probes.
      // Negotiate its unchanged read-only contract; never replay a mutation.
      reply = await device.command(COMMAND.CLASSIC_INSPECT, payload);
    }
    if (reply.length < 11 || reply[3] > 2 || reply[4] > 2) {
      throw new Error('Invalid Classic inspection reply');
    }
    if (reply.length !== 11) {
      if (
        !modelEvidence ||
        reply.length < 13 ||
        reply[11] !== 1 ||
        reply[12] > 64 ||
        reply.length !== 13 + reply[12]
      ) {
        throw new Error('Invalid Classic model evidence');
      }
      const model = decodeClassicAts(reply.slice(13), card);
      observation.modelEvidence = true;
      if (model.ats.length) {
        observation.ats = model.ats;
      }
      if (model.type) {
        observation.type = model.type;
      }
    }
    const key = Array.from(reply.slice(5, 11));
    if (reply[4]) {
      if (!ESSENTIALS.some((candidate) => candidate.every((byte, index) => byte === key[index]))) {
        throw new Error('Device returned an unsubmitted key');
      }
      observation.credentials.push({ key, keyB: reply[4] === 2 });
    } else if (key.some((byte) => byte !== 0)) {
      throw new Error('Invalid Classic inspection credential');
    }
    observation.magic = decodeClassicMagic(reply.slice(0, 3));
    observation.prng = ['Static', 'Weak', 'Hard'][reply[3]];
  } catch (error) {
    if (device.signal?.aborted || ![0x67, 0x69].includes(error.status)) {
      throw error;
    }
    observation.unavailable = true;
  }
  return observation;
}
