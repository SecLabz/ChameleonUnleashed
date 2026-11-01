import { COMMAND } from './commands.mjs';
import { hex } from './transport.mjs';
import {
  DESFIRE_STATES,
  allowsPublicRead,
  createSnapshot,
  desfireDocument,
  desfireLayout,
  littleEndianBytes,
  readLittleEndian
} from '../documents/desfire.mjs';

const unknownKeys = () => ({ status: 3, settings: [], versions: [] });

export async function readDesfire(device, card, onUpdate = () => {}) {
  const activation = Array.from(await device.command(COMMAND.DESFIRE, [1, card.uid.length, ...card.uid]));
  const uidLength = activation[0];
  if (
    ![4, 7, 10].includes(uidLength) ||
    activation.length < uidLength + 5 ||
    activation.length !== uidLength + 5 + activation[uidLength + 4] ||
    hex(activation.slice(1, uidLength + 1)) !== hex(card.uid)
  ) {
    throw new Error('DESFire identity changed');
  }

  const exchange = async (command, params = []) => {
    const reply = Array.from(await device.command(COMMAND.DESFIRE, [2, command, ...params]));
    if (!reply.length) {
      throw new Error('Empty DESFire response');
    }

    const status = reply[0];
    let state;
    switch (status) {
      case 0:
        state = 0;
        break;
      case 0x9d:
      case 0xae:
        state = 1;
        break;
      case 0x1c:
        state = 2;
        break;
      default:
        throw new Error(`DESFire status ${status.toString(16).padStart(2, '0')}`);
    }
    return { state, data: reply.slice(1) };
  };

  const required = async (command, params = []) => {
    const response = await exchange(command, params);
    if (response.state) {
      throw new Error(`DESFire ${DESFIRE_STATES[response.state]}`);
    }
    return response.data;
  };

  const keys = async () => {
    const response = await exchange(0x45);
    if (response.state) {
      return { status: response.state, settings: [], versions: [] };
    }
    if (response.data.length !== 2 || (response.data[1] & 15) > 14) {
      throw new Error('Invalid DESFire keys');
    }

    const versions = [];
    for (let keyIndex = 0; keyIndex < (response.data[1] & 15); keyIndex++) {
      const keyVersion = await exchange(0x64, [keyIndex]);
      if (keyVersion.state) {
        break;
      }
      if (keyVersion.data.length !== 1) {
        throw new Error('Invalid DESFire key version');
      }
      versions.push(...keyVersion.data);
    }
    return { status: 0, settings: response.data, versions };
  };

  const version = await required(0x60);
  if (version.length !== 28 || version.slice(0, 3).join() !== '4,1,1') {
    throw new Error('This ISO-DEP tag is not a supported DESFire card');
  }

  const capture = {
    uid: activation.slice(1, uidLength + 1),
    atqa: activation.slice(uidLength + 1, uidLength + 3),
    sak: activation[uidLength + 3],
    ats: activation.slice(uidLength + 5),
    version,
    free_memory: null,
    keys: unknownKeys(),
    applications_state: 3,
    applications: []
  };
  let lastSnapshot = createSnapshot(capture);
  let document = desfireDocument(lastSnapshot, `DESFire-${hex(capture.uid)}`);

  const update = (phase) => {
    lastSnapshot = createSnapshot(capture);
    document = desfireDocument(lastSnapshot, document.name);
    onUpdate(document, phase);
  };

  onUpdate(document, 'Reading public DESFire information…');

  try {
    const freeMemory = await exchange(0x6e);
    if (!freeMemory.state) {
      if (freeMemory.data.length !== 3) {
        throw new Error('Invalid free memory');
      }
      capture.free_memory = readLittleEndian(freeMemory.data);
    }

    capture.keys = await keys();
    update('Reading applications…');

    const applications = await exchange(0x6a);
    capture.applications_state = applications.state;
    if (!applications.state) {
      if (applications.data.length % 3 || applications.data.length / 3 > 28) {
        throw new Error('Invalid DESFire directory');
      }
      for (let offset = 0; offset < applications.data.length; offset += 3) {
        capture.applications.push({
          aid: applications.data.slice(offset, offset + 3),
          keys: unknownKeys(),
          files_state: 3,
          files: []
        });
      }

      update('Reading public applications…');
      let totalFiles = 0;
      for (const application of capture.applications) {
        await required(0x5a, application.aid);
        application.keys = await keys();
        update(`Application ${hex(application.aid)}`);

        const files = await exchange(0x6f);
        application.files_state = files.state;
        if (files.state) {
          update('File directory unavailable');
          continue;
        }

        totalFiles += files.data.length;
        if (files.data.length > 32 || totalFiles > 64) {
          throw new Error('DESFire capture exceeds 64 files');
        }

        application.files = files.data.map((id) => ({
          id,
          settings_state: 3,
          settings: [],
          data_state: 3,
          data: []
        }));
        update('Reading files…');

        for (const file of application.files) {
          const response = await exchange(0xf5, [file.id]);
          file.settings_state = response.state;
          if (response.state) {
            update('File settings unavailable');
            continue;
          }

          file.settings = response.data;
          const fileLayout = desfireLayout(file.settings);
          if (!allowsPublicRead(file.settings)) {
            file.data_state = 1;
            update('Protected file retained');
            continue;
          }
          if (!fileLayout) {
            file.data_state = 2;
            update('Unsupported file retained');
            continue;
          }
          update('File settings read');
        }
      }
    }

    let capturedBytes = createSnapshot(capture).memory.length;
    for (const application of capture.applications) {
      const hasUnreadFiles = application.files.some(
        (file) => file.settings_state === 0 && file.data_state === 3
      );
      if (!hasUnreadFiles) {
        continue;
      }

      await required(0x5a, application.aid);
      for (const file of application.files) {
        if (file.settings_state !== 0 || file.data_state !== 3) {
          continue;
        }

        const [command, fileBytes, unitBytes] = desfireLayout(file.settings);
        if (capturedBytes + fileBytes > 8192 || unitBytes > 511) {
          throw new Error('DESFire file exceeds capture capacity');
        }

        const data = [];
        if (command === 0x6c) {
          data.push(...(await required(command, [file.id])));
        } else {
          for (let unitOffset = 0; unitOffset < fileBytes / unitBytes; ) {
            const unitCount = Math.min(
              Math.max(1, Math.floor(128 / unitBytes)),
              fileBytes / unitBytes - unitOffset
            );
            const chunk = await required(command, [
              file.id,
              ...littleEndianBytes(unitOffset, 3),
              ...littleEndianBytes(unitCount, 3)
            ]);
            if (chunk.length !== unitCount * unitBytes) {
              throw new Error('Invalid DESFire read length');
            }
            data.push(...chunk);
            unitOffset += unitCount;
          }
        }

        if (data.length !== fileBytes) {
          throw new Error('Invalid DESFire file length');
        }
        capturedBytes += data.length;
        file.data = data;
        file.data_state = 0;
        update(`Read file ${file.id}`);
      }
    }

    lastSnapshot = createSnapshot(capture, true);
    document = desfireDocument(lastSnapshot, document.name);
    onUpdate(document, 'Public read complete');
  } catch (error) {
    lastSnapshot = { ...lastSnapshot, complete: false, error: error.message };
    document = desfireDocument(lastSnapshot, document.name);
    onUpdate(document, `Read interrupted · ${error.message}`);
  }

  return document;
}
