import { hex } from '../device/transport.mjs';

const targetId = (target) => `${target.sector}:${target.is_key_b}`;

// Screen reuse once several distinct keys make failed RF authentications
// expensive. Parity filtering works for both weak and hard PRNGs. Evidence belongs to
// this read session; a local miss is never a physical authentication failure.
export function createKeyReuseFilter({ call, collect, uid }) {
  const records = new Map();
  const disabled = new Set();

  function sourceFor(summary, target) {
    if (!['Weak', 'Hard'].includes(summary?.prng) || disabled.has(targetId(target))) {
      return null;
    }
    const keys = summary.mapping.flatMap((entry) => [entry.key_a, entry.key_b]).filter(Boolean);
    if (new Set(keys.map(hex)).size < 3) {
      return null;
    }
    for (const sector of [target.sector, ...summary.mapping.keys()]) {
      const entry = summary.mapping[sector];
      if (entry.key_a) {
        return { sector, is_key_b: false, key: entry.key_a };
      }
      if (entry.key_b && entry.origin_b !== 'Trailer') {
        return { sector, is_key_b: true, key: entry.key_b };
      }
    }
    return null;
  }

  return {
    eligible: (summary, target) => !!sourceFor(summary, target),
    async test(summary, target, key) {
      const source = sourceFor(summary, target);
      if (!source) {
        return null;
      }
      const id = targetId(target);
      if (!records.has(id)) {
        try {
          records.set(id, await collect(source, target, 2));
        } catch (error) {
          if (error.status !== 6 && error.status !== 0x69) {
            throw error;
          }
          this.disable(target);
          return null;
        }
      }
      const result = await call({
        op: 'filter_dictionary',
        uid: uid(),
        keys: [key],
        observations: records.get(id),
        weak_prng: false
      });
      return result.candidates.length > 0;
    },
    take(target) {
      const id = targetId(target);
      const prefix = records.get(id) || [];
      records.delete(id);
      return prefix;
    },
    disable(target) {
      disabled.add(targetId(target));
      records.delete(targetId(target));
    }
  };
}
