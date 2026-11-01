export async function filterDictionary({ call, uid, keys, collect }) {
  const observations = [];
  const plan = await call({ op: 'recovery_plan', static_nonce: false, prefix_count: 0 });
  let count = plan.dictionary_start_count;

  for (;;) {
    observations.push(...(await collect(count)));
    const result = await call({
      op: 'filter_dictionary',
      uid,
      keys,
      observations,
      weak_prng: false,
      adaptive: true
    });
    if (!result.next_count) {
      return result;
    }
    count = result.next_count;
  }
}
