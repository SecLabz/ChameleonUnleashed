import { Device, hex } from './device.mjs';
import { scanNfcA } from './hf-reader.mjs';
import { withDevice } from './transport.mjs';
import { withRuntime } from '../runtime/runtime.mjs';
import { ESSENTIALS } from '../documents/classic-keys.mjs';
import { classicObservationMatches, maintenanceMethod } from '../documents/classic-magic.mjs';
import { repairBlock0, factoryTrailer, maintenanceSectors } from '../documents/classic-maintenance.mjs';
import { inspectClassic } from './classic-inspect.mjs';

const MODELS = { 9: 5, 8: 1, 16: 6, 24: 7 };
const equal = (left, right) => left.join() === right.join();
const retryable = (error) => error.status === 6;

export async function maintainClassic(format, source, options, signal, onProgress, confirm) {
  const device = new Device(signal);
  let completed = 0;
  let writesStarted = false;
  try {
    return await withDevice(device, async () => {
      await device.reserve();
      let card;
      try {
        card = await scanNfcA(device);
        device.card = card;
      } catch (error) {
        if (![1, 2, 3, 4, 5].includes(error.status) || format || !options.method || !options.model) {
          throw Object.assign(
            new Error(
              `${error.message}. ${
                format
                  ? 'Repair an unidentified card before formatting it.'
                  : 'Use Card not detected to supply the known generation and model.'
              }`
            ),
            { setup: !format }
          );
        }
      }
      let observation;
      if (card && (!options.method || options.model === 6 || source?.type === 1002)) {
        observation = classicObservationMatches(source?.classicObservation, card)
          ? source.classicObservation
          : await inspectClassic(device, card, onProgress, { modelEvidence: true });
        if (observation.ats) {
          card.ats = observation.ats;
        }
      }
      // Method 3 is standard authenticated writing; formatting never writes block 0.
      const method =
        options.method ||
        maintenanceMethod(observation?.magic) ||
        (format || options.repairIdentity === false ? 3 : null);
      if (!method) {
        throw Object.assign(
          new Error(
            'Identification repair requires a known rewritable card. Supply its generation under Card not detected.'
          ),
          { setup: true }
        );
      }
      const known2k = source?.type === 1002 && classicObservationMatches({ card: source.card }, card);
      const detectedModel =
        observation?.type === 1002 || (known2k && card?.sak === 8) ? 6 : MODELS[card?.sak & ~0x20];
      const model = options.model || detectedModel;
      const sak = Number(Object.keys(MODELS).find((sak) => MODELS[sak] === model));
      if (!model) {
        throw Object.assign(
          new Error(
            'A MIFARE Classic card is required. Supply its model only when repairing an unidentified card.'
          ),
          { setup: !format }
        );
      }
      if (card && (detectedModel !== model || ![4, 7].includes(card.uid.length))) {
        if (!format && options.method && options.model) {
          card = null;
        } else {
          throw new Error('Selected model differs from the card.');
        }
      }
      const { geometry, keys } = await withRuntime(async (call) => {
        const geometry = await call({ op: 'classic_geometry', sak });
        if (method !== 3) {
          return { geometry, keys: [] };
        }
        const plan = await call({
          op: 'dictionary',
          text: options.dictionary || '',
          anchor: null,
          profile: 'Essentials'
        });
        return { geometry, keys: plan.passes.flatMap((pass) => pass.keys) };
      }, signal);
      const known = observation?.credentials?.map((credential) => credential.key) || [];
      for (const mapping of source?.recoveredKeys || []) {
        for (const key of [mapping.key_a, mapping.key_b]) {
          if (key) {
            known.push(key);
          }
        }
      }
      if (options.key) {
        known.unshift(options.key);
      }
      const candidates = [
        ...new Map([...known, ...keys, ...ESSENTIALS].map((key) => [hex(key), key])).values()
      ];
      const target = {
        method,
        model,
        capacityCheck:
          method === 3 &&
          model === 6 &&
          card?.sak === 8 &&
          !(options.method && options.model && observation?.type !== 1002),
        uid: card?.uid || Array(options.uid?.length || 4).fill(0),
        unbound: !card
      };
      const reset = maintenanceSectors(options.sectors, geometry.sectors.length);
      const units = format
        ? Array.from({ length: geometry.blocks }, (_, index) => index)
        : [0, ...reset.map((sector) => geometry.sectors[sector].trailer)];
      const preferred = new Map();
      const reading = source?.classicRead;
      const sectorKeys = classicObservationMatches(reading, card) ? reading.keys : [];
      const credentialsFor = (sector) => {
        if (method !== 3) {
          return [null];
        }
        const mapping = sectorKeys[sector];
        const credentials = [preferred.get(sector)];
        if (mapping?.key_a) {
          credentials.push({ key: mapping.key_a, keyB: false });
        }
        if (mapping?.key_b && mapping.origin_b !== 'Trailer') {
          credentials.push({ key: mapping.key_b, keyB: true });
        }
        for (const key of candidates) {
          credentials.push({ key, keyB: false }, { key, keyB: true });
        }
        const unique = new Map();
        for (const credential of credentials.filter(Boolean)) {
          const id = `${hex(credential.key)}:${credential.keyB}`;
          if (!unique.has(id)) {
            unique.set(id, credential);
          }
        }
        return [...unique.values()];
      };
      const plan = [];
      let initial;
      let identityCredential;
      let sectorRead;
      for (const index of units) {
        device.check();
        const sector = geometry.sectors.findIndex(
          (group) => index >= group.first && index < group.first + group.count
        );
        const group = geometry.sectors[sector];
        const trailer = group.trailer === index;
        onProgress(`Checking sector ${sector} · block ${index}…`);
        const credentials = credentialsFor(sector);
        let usable = false;
        for (const credential of credentials) {
          let observed;
          try {
            if (format && method === 3 && sectorRead?.sector !== sector) {
              const blocks = await device.readSector(sector, credential.key, credential.keyB, geometry);
              sectorRead = { sector, credential, blocks };
            }
            const block =
              sectorRead?.sector === sector && sectorRead.credential === credential
                ? sectorRead.blocks[index - group.first]
                : null;
            observed = block ? { card, block } : await device.maintenanceUnit(target, index, credential);
          } catch (error) {
            if (!retryable(error) && error.status !== 2) {
              throw error;
            }
            continue;
          }
          card = observed.card;
          const before = observed.block;
          if (index === 0) {
            if (!initial) {
              initial = observed;
              identityCredential = credential;
            }
            if (format || options.repairIdentity === false) {
              preferred.set(sector, credential);
              usable = true;
              break;
            }
          }
          let after = Array(16).fill(0);
          if (index === 0) {
            const observedSak = model === 6 && [8, 16].includes(card.sak) ? card.sak : undefined;
            after = repairBlock0(before, options.uid || target.uid, geometry, observedSak);
          } else if (trailer) {
            after = factoryTrailer(format && sector === geometry.sectors.length - 1);
          }
          // Authenticated reads hide Key A. Prove it with the successful
          // authentication, then compare the factory access bytes and readable Key B.
          const unchanged =
            trailer && method === 3
              ? !credential.keyB &&
                equal(credential.key, after.slice(0, 6)) &&
                equal(before.slice(6), after.slice(6))
              : equal(before, after);
          if (unchanged) {
            preferred.set(sector, credential);
            usable = true;
            break;
          }
          try {
            await device.maintenanceUnit(target, index, credential, { before, after }, true);
          } catch (error) {
            if (!retryable(error) && error.status !== 0x75) {
              throw error;
            }
            continue;
          }
          preferred.set(sector, credential);
          plan.push({ index, before, after, credential, trailer });
          usable = true;
          break;
        }
        if (!usable) {
          if (index === 0 && !initial) {
            throw new Error('No usable sector-0 key. No writes sent.');
          }
          throw new Error(`Block ${index}: no usable key or write permission. No writes sent.`);
        }
      }
      plan.sort((left, right) => Number(left.trailer) - Number(right.trailer));
      const accepted = await confirm({ card, method, geometry, plan, reset }, signal);
      if (!accepted || !plan.length) {
        let message = format ? 'Already formatted · no writes sent' : 'No repair needed · no writes sent';
        if (plan.length) {
          message = 'Maintenance cancelled · no writes sent';
        }
        onProgress(message);
        return false;
      }
      for (const unit of plan) {
        onProgress(
          `${format ? 'Formatting' : 'Repairing'} block ${unit.index} · ${completed}/${plan.length} verified…`
        );
        writesStarted = true;
        await device.maintenanceUnit(target, unit.index, unit.credential, unit);
        if (unit.index === 0) {
          target.uid = unit.after.slice(0, target.uid.length);
        }
        completed++;
      }
      let finalCredential = identityCredential;
      if (method === 3 && (format || reset.includes(0))) {
        finalCredential = { key: Array(6).fill(255), keyB: false };
      }
      const final = await device.maintenanceUnit(target, 0, finalCredential);
      const expected = plan.find((unit) => unit.index === 0)?.after || initial.block;
      if (!equal(final.block, expected)) {
        throw new Error('Final block-0 verification failed. Inspect the tag before retrying.');
      }
      onProgress(
        `${format ? 'Factory format' : 'Repair'} verified · ${completed} blocks · read again to refresh memory`
      );
      return true;
    });
  } catch (error) {
    throw Object.assign(new Error(`${completed} blocks written and verified. ${error.message}`), {
      setup: error.setup
    });
  } finally {
    if (writesStarted && source) {
      delete source.classicRead;
      delete source.recoveredKeys;
      delete source.classicObservation;
    }
  }
}
