// Independent USB wire snapshots for the physical bench, never a reader/RF lease.
export async function usbSlotSnapshot(request, deviceURL, indices = [0, 1, 2, 3, 4, 5, 6, 7]) {
  const body = async (response) => {
    if (response.status() !== 200) {
      throw new Error(`USB snapshot failed: HTTP ${response.status()}`);
    }
    return [...(await response.body())];
  };
  const metadata = {};
  for (const index of indices) {
    metadata[index] = await body(await request.get(`${deviceURL}/api/slot/${index}`, { timeout: 10000 }));
  }
  const lease = await request.post(deviceURL + '/api/slots/session', {
    data: Buffer.alloc(0),
    timeout: 10000
  });
  if (lease.status() !== 200) {
    throw new Error(`USB snapshot reservation failed: HTTP ${lease.status()}`);
  }
  const { session } = await lease.json();
  const images = [];
  try {
    for (const slot of indices) {
      for (const [sense, offset] of [
        [2, 0],
        [1, 2]
      ]) {
        const type = metadata[slot][offset] * 256 + metadata[slot][offset + 1];
        if (!type) {
          continue;
        }
        const opened = await body(
          await request.post(`${deviceURL}/api/slots/open?id=${session}`, {
            data: Buffer.from([slot, sense]),
            timeout: 10000
          })
        );
        if (opened.length !== 6) {
          throw new Error('Invalid USB snapshot header');
        }
        const size = opened[0] * 256 + opened[1];
        if (size < 9 || size > 4500) {
          throw new Error('Invalid USB snapshot size');
        }
        const bytes = [];
        for (let start = 0; start < size; start += 512) {
          const count = Math.min(512, size - start);
          const part = await body(
            await request.post(`${deviceURL}/api/slots/read?id=${session}`, {
              data: Buffer.from([start >> 8, start & 255, count >> 8, count & 255]),
              timeout: 10000
            })
          );
          if (part.length !== count) {
            throw new Error('Truncated USB snapshot');
          }
          bytes.push(...part);
        }
        images.push({ slot, sense, type, opened, bytes });
      }
    }
  } finally {
    const ended = await request.post(`${deviceURL}/api/end?id=${session}`, {
      data: Buffer.alloc(0),
      timeout: 10000
    });
    if (ended.status() !== 200) {
      throw new Error(`USB snapshot release failed: HTTP ${ended.status()}`);
    }
  }
  return { metadata, images };
}
