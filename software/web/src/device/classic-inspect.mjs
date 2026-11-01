import { ESSENTIALS } from '../documents/classic-keys.mjs';
import { decodeClassicMagic } from '../documents/classic-magic.mjs';
import { COMMAND } from './commands.mjs';

export async function inspectClassic(device, card, onProgress = () => {}) {
  const observation = {
    card: { uid: [...card.uid], atqa: [...card.atqa], sak: card.sak },
    credentials: []
  };
  try {
    onProgress('Checking Classic capabilities…');
    const reply = await device.command(COMMAND.CLASSIC_INSPECT, [
      card.uid.length,
      ...card.uid,
      ...Array(10 - card.uid.length).fill(0),
      ...[...card.atqa].reverse(),
      card.sak,
      ESSENTIALS.length,
      ...ESSENTIALS.flat()
    ]);
    if (reply.length !== 11 || reply[3] > 2 || reply[4] > 2) {
      throw new Error('Invalid Classic inspection reply');
    }
    const key = Array.from(reply.slice(5));
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
