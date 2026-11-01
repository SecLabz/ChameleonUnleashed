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

  async function setCaptureSession(enabled) {
    await device.command(COMMAND.CAPTURE_SESSION, [Number(enabled)]);
    captureSessionOpen = enabled;
  }

  async function acquire() {
    check();

    const count = Math.min(16, budget - capturedCount);
    const records = await device.collectNonces(source, target, count, geometry);
    capturedCount += records.length;
    pendingObservations.push(...records);
    update({ phase: `Hardnested acquisition · ${capturedCount}/${budget}` });
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
      while (acquiring && !workerSettled && capturedCount < budget) {
        await acquire();
      }
    } catch (error) {
      acquisitionError = error;
    }

    const result = await workerResult;

    if (acquisitionError) {
      throw acquisitionError;
    }

    if (result.error) {
      throw result.error;
    }

    return result.value;
  }

  function pushObservations(records, acquiring = true) {
    return processWithAcquisition({ op: 'hardnested_push', job, observations: records }, acquiring);
  }

  try {
    await setCaptureSession(true);
    let status;

    while (pendingObservations.length || capturedCount < budget) {
      check();

      if (!pendingObservations.length) {
        await acquire();
      }

      status = await pushObservations(pendingObservations.splice(0, 16));

      // Old buffered observations cannot establish that reselection failed.
      const observedAfterReselection = reselectedAfterCount === null || status.count > reselectedAfterCount;
      const needsMoreDiversity = !status.diverse && status.count >= 32;

      if (needsMoreDiversity && observedAfterReselection) {
        if (reselectedAfterCount !== null) {
          return 'NoDiversity';
        }

        reselectedAfterCount = capturedCount;
        await setCaptureSession(false);
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
