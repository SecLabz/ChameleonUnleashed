import { COMMAND } from '../device/commands.mjs';

// The Worker owns reduction state; this adapter owns bounded RF acquisition.
export async function recoverHardnested({
  call,
  device,
  source,
  target,
  geometry,
  uid,
  budget,
  verify,
  check,
  update
}) {
  const { job } = await call({
    op: 'hardnested_new',
    uid,
    block: geometry.sectors[target.sector].first,
    key_b: target.is_key_b,
    budget
  });

  let capturedCount = 0;
  const pendingObservations = [];
  let captureSessionOpen = false;
  let reselectedAfterCount = null;
  let stream = null;
  let streamingMode;

  function progress() {
    update({ phase: `Hardnested acquisition · ${capturedCount + (stream?.buffered ?? 0)}/${budget}` });
  }

  function retain(records) {
    capturedCount += records.length;
    pendingObservations.push(...records);
    if (records.length) {
      progress();
    }
  }

  async function setCaptureSession(enabled) {
    if (!enabled && stream) {
      const current = stream;
      try {
        await current.stop();
      } finally {
        stream = null;
        retain(current.drain());
        captureSessionOpen = false;
      }
      return;
    }
    if (enabled && streamingMode !== false && typeof device.startNonceStream === 'function') {
      stream = device.startNonceStream(source, target, budget - capturedCount, geometry, progress);
      if (streamingMode === true && !stream) {
        throw new Error('Nonce stream disconnected');
      }
      streamingMode = !!stream;
      if (stream) {
        captureSessionOpen = true;
        return;
      }
    }
    await device.command(COMMAND.CAPTURE_SESSION, [Number(enabled)]);
    captureSessionOpen = enabled;
  }

  async function acquire() {
    check();

    const count = Math.min(16, budget - capturedCount);
    const records = stream
      ? await stream.read()
      : await device.collectNonces(source, target, count, geometry, false, uid);
    retain(records);
    check();
    if (!records.length) {
      throw new Error('Nonce acquisition ended before its budget');
    }
  }

  async function processWithAcquisition(request, acquiring = true) {
    // Settle both Worker and RF work before verification, fallback or cleanup.
    // This also applies when the Worker fails or the user requests Stop.
    let workerSettled = false;
    let acquisitionError;
    const workerResult = call(request).then(
      (value) => {
        workerSettled = true;
        return { value };
      },
      (error) => {
        workerSettled = true;
        return { error };
      }
    );

    try {
      // Native notifications keep RF acquisition running independently. Do not
      // wait for another radio batch when the Worker can process buffered data.
      while (!stream && acquiring && !workerSettled && capturedCount < budget) {
        await acquire();
      }
    } catch (error) {
      acquisitionError = error;
    }

    const result = await workerResult;

    if (stream?.error && !acquisitionError) {
      acquisitionError = stream.error;
    }

    if (acquisitionError) {
      throw acquisitionError;
    }

    if (result.error) {
      throw result.error;
    }

    return result.value;
  }

  try {
    await setCaptureSession(true);
    let status;

    while (pendingObservations.length || capturedCount < budget) {
      check();

      if (!pendingObservations.length) {
        await acquire();
      }

      const observations = pendingObservations.splice(0, 16);
      // Let reduction catch up before acquiring another page.
      status = await processWithAcquisition(
        { op: 'hardnested_push', job, observations },
        pendingObservations.length < 16
      );

      // Old buffered observations cannot establish that reselection failed.
      const observedAfterReselection = reselectedAfterCount === null || status.count > reselectedAfterCount;
      const needsMoreDiversity = !status.diverse && status.count >= 32;

      if (needsMoreDiversity && observedAfterReselection) {
        if (reselectedAfterCount !== null) {
          return 'NoDiversity';
        }

        await setCaptureSession(false);
        reselectedAfterCount = capturedCount;
        if (capturedCount === budget) {
          return 'NoDiversity';
        }
        await setCaptureSession(true);
      }

      if (!status.early) {
        continue;
      }

      const result = await processWithAcquisition({ op: 'hardnested_early', job });

      if (!result.key) {
        continue;
      }

      // Keep buffered observations while checking the candidate. A rejection
      // rebuilds reduction state and then ingests the retained observations.
      await setCaptureSession(false);
      const candidate = result.key.match(/../g).map((byte) => parseInt(byte, 16));
      const key = await verify(target, [candidate]);

      if (key) {
        return { Verified: key };
      }

      await call({ op: 'hardnested_reject', job });

      if (capturedCount < budget) {
        await setCaptureSession(true);
      }
    }

    if (status.covered !== 256) {
      return 'ResourceLimited';
    }

    if (captureSessionOpen) {
      await setCaptureSession(false);
    }

    update({ phase: 'Solving Hardnested in your browser…' });
    const result = await call({ op: 'hardnested_finish', job });

    if (!result.key) {
      return 'NoVerifiedKey';
    }

    const candidate = result.key.match(/../g).map((byte) => parseInt(byte, 16));
    const key = await verify(target, [candidate]);

    return key ? { Verified: key } : 'NoVerifiedKey';
  } finally {
    try {
      if (captureSessionOpen) {
        await setCaptureSession(false);
      }
    } finally {
      try {
        await call({ op: 'hardnested_dispose', job });
      } catch {
        // Stop disposes the Worker and its jobs.
      }
    }
  }
}
