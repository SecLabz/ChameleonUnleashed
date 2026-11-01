import { COMMAND } from '../device/commands.mjs';
import { hardnestedTablesRequest } from '../runtime/nfc-wasm/index.mjs';
import { createRuntime } from '../runtime/runtime.mjs';
import { Device, hex } from '../device/device.mjs';
import { getTables } from '../runtime/assets.mjs';
import { createHardnestedRuntime } from '../runtime/hardnested-runtime.mjs';
import { recoverHardnested } from './hardnested.mjs';
import { recoverDarkside } from './darkside.mjs';
import { filterDictionary as filterCandidates } from './dictionary.mjs';

const verified = (key) => (key ? { Verified: key } : 'NoVerifiedKey');

const isRecoverable = (error) => error.status === 6 || error.status === 0x69;

export async function runAutoPwn({
  dictionary = '',
  signal,
  onUpdate = () => {},
  device = new Device(signal),
  workerFactory,
  initialCard,
  expectedUid
} = {}) {
  let engine;
  let hardnestedEngine;
  let recoverySession;
  let snapshot;
  let geometry;
  let card;
  const memory = [];

  const update = (extra) => {
    geometry?.sectors.forEach((sector, index) => {
      const trailer = memory[sector.trailer];
      const keys = snapshot?.summary.mapping[index];
      if (trailer && keys) {
        if (keys.key_a) {
          trailer.splice(0, 6, ...keys.key_a);
        }

        if (keys.key_b) {
          trailer.splice(10, 6, ...keys.key_b);
        }
      }
    });

    onUpdate({ card, geometry, summary: snapshot?.summary, memory, ...extra });
  };

  const check = () => {
    if (signal?.aborted) {
      throw new Error('Operation cancelled');
    }
  };

  const call = (request) => {
    check();
    return engine.request(request, { signal });
  };

  let nonceKeys = [];
  const testedKeysByTarget = new Map();
  const targetId = (target) => `${target.sector}:${target.is_key_b}`;
  const untestedKeys = (target, keys) =>
    keys.filter((key) => !testedKeysByTarget.get(targetId(target))?.has(hex(key)));

  async function verify(target, candidates, onTested = () => {}) {
    check();

    const selected = untestedKeys(target, candidates);
    if (!selected.length) {
      return null;
    }

    return device.findKey(target.sector, target.is_key_b, selected, geometry, (keys) => {
      const tested = testedKeysByTarget.get(targetId(target)) ?? new Set();
      keys.forEach((key) => tested.add(hex(key)));
      testedKeysByTarget.set(targetId(target), tested);
      onTested(keys);
    });
  }

  async function collect(source, target, count, chained = false) {
    check();

    return device.collectNonces(source, target, count, geometry, chained);
  }

  async function filterDictionary(source, target) {
    const result = await filterCandidates({
      call,
      uid: card.uid,
      keys: untestedKeys(target, nonceKeys),
      collect: (count) => collect(source, target, count)
    });
    const tested = [];
    const key = await verify(target, result.candidates, (keys) => tested.push(...keys));
    let outcome = 'ResourceLimited';

    if (key) {
      outcome = { VerifiedDictionary: key };
    } else if (result.exhausted) {
      outcome = 'NoVerifiedKey';
    }

    return { Nested: { outcome, tested } };
  }

  async function recoverNested(source, target, staticNonce) {
    const tested = [];
    let job;

    try {
      const observations = [];
      let plan = await call({ op: 'recovery_plan', static_nonce: staticNonce, prefix_count: 0 });

      if (untestedKeys(target, nonceKeys).length) {
        for (const step of plan.prefix) {
          observations.push(...(await collect(source, target, step.count, step.chained)));
        }

        const result = await call({
          op: 'filter_dictionary',
          uid: card.uid,
          keys: untestedKeys(target, nonceKeys),
          observations,
          weak_prng: !staticNonce
        });
        const key = await verify(target, result.candidates, (keys) => tested.push(...keys));

        if (key) {
          return { outcome: { VerifiedDictionary: key }, tested };
        }
      }

      const calibrationRecords = await collect(source, source, plan.calibration_count);
      const { calibration } = await call({
        op: 'calibrate',
        uid: card.uid,
        key: source.key,
        observations: calibrationRecords
      });
      plan = await call({
        op: 'recovery_plan',
        static_nonce: staticNonce,
        prefix_count: observations.length
      });

      if (plan.target_count) {
        if (staticNonce) {
          for (const step of plan.prefix) {
            observations.push(...(await collect(source, target, step.count, step.chained)));
          }
        } else {
          observations.push(...(await collect(source, target, plan.target_count)));
        }
      }

      let request;

      if (staticNonce) {
        request = { op: 'static_new', uid: card.uid, observations, distance: calibration[1] };
      } else {
        request = { op: 'nested_new', uid: card.uid, observations, calibration, tolerance: 128 };
      }

      ({ job } = await call(request));

      for (;;) {
        const { done, candidates } = await call({ op: 'nested_next', job });

        if (done) {
          return { outcome: 'NoVerifiedKey', tested };
        }

        const key = await verify(target, candidates, (keys) => tested.push(...keys));

        if (key) {
          return { outcome: verified(key), tested };
        }
      }
    } catch (error) {
      if (signal?.aborted) {
        throw error;
      }

      if (isRecoverable(error) || /ResourceLimit|Calibration failed|NoDiversity/.test(error.message)) {
        let outcome = 'ResourceLimited';

        if (/Calibration/.test(error.message)) {
          outcome = 'CalibrationFailed';
        } else if (/NoDiversity/.test(error.message)) {
          outcome = 'NoDiversity';
        }

        return { outcome, tested };
      }

      throw error;
    } finally {
      if (job) {
        try {
          await engine.request({ op: 'nested_dispose', job });
        } catch {
          /* A cancelled Worker has already released its state. */
        }
      }
    }
  }

  async function runHardnested({ source, target, budget }) {
    if (!hardnestedEngine) {
      update({ phase: 'Loading Hardnested module and tables…' });

      try {
        const tables = await getTables(signal);
        hardnestedEngine = await createHardnestedRuntime(signal, workerFactory);
        await hardnestedEngine.request(hardnestedTablesRequest(tables), { signal });
      } catch (error) {
        hardnestedEngine?.dispose();
        hardnestedEngine = null;

        if (signal?.aborted) {
          throw error;
        }

        update({ phase: error.message });
        return { Hardnested: 'ResourceLimited' };
      }
    }

    const hardnestedCall = (request) => {
      check();
      return hardnestedEngine.request(request, { signal });
    };

    return {
      Hardnested: await recoverHardnested({
        call: hardnestedCall,
        device,
        source,
        target,
        geometry,
        uid: card.uid,
        budget,
        verify,
        check,
        update
      })
    };
  }

  async function execute(action) {
    if (action === 'ProbePrng') {
      const data = await device.command(COMMAND.CLASSIC_PRNG);

      if (data.length !== 1 || data[0] > 2) {
        throw new Error('Invalid PRNG reply');
      }

      return { Prng: ['Static', 'Weak', 'Hard'][data[0]] };
    }

    const [kind, args] = Object.entries(action)[0];
    update({ phase: `${kind} · sector ${args.sector ?? args.target?.sector ?? ''}` });

    switch (kind) {
      case 'TestDictionary':
      case 'VerifyCandidates':
        return { CandidateKey: await verify(args, args.candidates) };
      case 'SweepKey':
        return { Sweep: { hit: !!(await verify(args, [args.key])), physically_tested: true } };
      case 'ExtractTrailer': {
        try {
          const block = await device.readBlock(geometry.sectors[args.sector].trailer, args.key_a, false);
          const { key } = await call({ op: 'trailer_key_b', block });
          return { TrailerKey: key };
        } catch (error) {
          if (error.status === 6) {
            return { TrailerKey: null };
          }

          throw error;
        }
      }
      case 'FilterDictionary':
        return filterDictionary(args.source, args.target);
      case 'RecoverNested':
        return { Nested: await recoverNested(args.source, args.target, false) };
      case 'RecoverStaticNested':
        return { StaticNested: await recoverNested(args.source, args.target, true) };
      case 'RecoverHardnested':
        return runHardnested(args);
      case 'RecoverDarkside':
        return {
          Darkside: await recoverDarkside({
            device,
            target: args.target,
            geometry,
            call,
            verify,
            check,
            update
          })
        };
      default:
        throw new Error(`Unsupported AutoPwn action ${kind}`);
    }
  }

  try {
    update({ phase: 'Loading shared recovery engine…' });
    engine = await createRuntime(signal, workerFactory);
    check();

    card = initialCard || (await device.begin());

    if (expectedUid && hex(card.uid) !== expectedUid) {
      throw new Error('The tag UID changed. Scan the current tag before recovery.');
    }

    geometry = await call({ op: 'classic_geometry', sak: card.sak });
    memory.length = geometry.blocks;
    memory.fill(null);

    const plan = await call({ op: 'dictionary', text: dictionary, anchor: null, profile: 'Extended' });
    nonceKeys = plan.nonce_keys;

    ({ session: recoverySession } = await call({
      op: 'autopwn_new',
      config: {
        uid: card.uid,
        sector_count: geometry.sectors.length,
        first_sector: 0,
        last_sector: geometry.sectors.length - 1,
        prng: 'Unknown',
        budget: 4096
      },
      passes: plan.passes,
      sources: []
    }));

    snapshot = await call({ op: 'autopwn_next', session: recoverySession });

    while (snapshot.action) {
      check();
      const result = await execute(snapshot.action);
      snapshot = await call({
        op: 'autopwn_submit',
        session: recoverySession,
        action_id: snapshot.action_id,
        result
      });
      update({ phase: 'Recovering keys…' });
    }

    for (let sector = 0; sector < geometry.sectors.length; sector++) {
      const keys = snapshot.summary.mapping[sector];
      const sectorGeometry = geometry.sectors[sector];

      for (const isKeyB of [false, true]) {
        const key = isKeyB ? keys.key_b : keys.key_a;

        if (!key || (isKeyB && keys.origin_b === 'Trailer')) {
          continue;
        }

        check();

        if (memory.slice(sectorGeometry.first, sectorGeometry.first + sectorGeometry.count).every(Boolean)) {
          break;
        }

        try {
          const blocks = await device.readSector(sector, key, isKeyB, geometry);
          blocks.forEach((bytes, index) => {
            if (bytes) {
              memory[sectorGeometry.first + index] = bytes;
            }
          });

          update({ phase: `Reading memory · sector ${sector + 1}/${geometry.sectors.length}` });

          for (let offset = 0; offset < sectorGeometry.count; offset++) {
            check();

            const block = sectorGeometry.first + offset;

            if (memory[block]) {
              continue;
            }

            try {
              memory[block] = await device.readBlock(block, key, isKeyB);
            } catch (error) {
              // RC522 reports a read NAK as a short/invalid frame (status 2).
              if (error.status !== 6 && error.status !== 2) {
                throw error;
              }
            }

            update({ phase: `Reading memory · block ${block + 1}/${geometry.blocks}` });
          }
        } catch (error) {
          if (error.status !== 6) {
            throw error;
          }
        }
      }
    }

    const complete = memory.every(Boolean);
    let dump = null;

    if (complete) {
      try {
        dump = (await call({ op: 'classic_export', uid: card.uid, sak: card.sak, memory: memory.flat() }))
          .text;
      } catch {
        /* Complete memory can still be retained when this .nfc identity is unsupported. */
      }
    }

    const resultLabel = complete ? 'Read complete' : 'Partial read · some blocks remain unread';

    update({
      phase: resultLabel,
      complete,
      dump
    });

    return { card, geometry, summary: snapshot.summary, memory, complete, dump };
  } catch (error) {
    const stopped = !!signal?.aborted;
    update({ phase: stopped ? 'Stopped · partial results retained' : error.message, error: !stopped });

    if (!stopped) {
      throw error;
    }

    return { card, geometry, summary: snapshot?.summary, memory, complete: false, dump: null };
  } finally {
    hardnestedEngine?.dispose();
    engine?.dispose();

    try {
      await device.end();
    } catch (error) {
      update({
        phase: `Device cleanup unconfirmed: ${error.message}. The device reservation expires automatically.`,
        error: true
      });
    }
  }
}
