import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { readFile } from 'node:fs/promises';

export async function startSimulation(
  executable,
  port,
  command,
  logfile,
  { restoreBin, classicStop = false, classicStopReport } = {}
) {
  if (classicStop && !restoreBin) {
    throw new Error('Classic simulation requires a full RAM backup');
  }
  const child = spawn(
    process.env.PYTHON || 'python3',
    [
      fileURLToPath(new URL('./pm3-simulation.py', import.meta.url)),
      resolve(executable),
      port,
      command,
      logfile
    ],
    {
      stdio: ['pipe', 'pipe', 'pipe'],
      detached: true,
      env: {
        ...process.env,
        CHAMELEON_PM3_SIM_RESTORE_BIN: restoreBin || '',
        CHAMELEON_PM3_CLASSIC_STOP_PREFLIGHT: '',
        ...(classicStopReport ? { CHAMELEON_PM3_CLASSIC_STOP_REPORT: classicStopReport } : {}),
        ...(classicStop
          ? { CHAMELEON_PM3_TEST_CLASSIC_STOP: '1', CHAMELEON_PM3_TEST_PERSISTENT_STOP: '1' }
          : {})
      }
    }
  );
  let output = '';
  let errors = '';
  child.stdin.on('error', (error) => {
    errors += `\nstdin: ${error.message}`;
  });
  let resolveReady;
  let rejectReady;
  const ready = new Promise((resolve, reject) => {
    resolveReady = resolve;
    rejectReady = reject;
  });
  child.stdout.on('data', (data) => {
    output += data;
    if (output.includes('"ready": true')) {
      resolveReady();
    }
  });
  child.stderr.on('data', (data) => {
    errors += data;
  });
  const closed = new Promise((resolve) => {
    child.once('error', (error) => {
      rejectReady(error);
      resolve({ error });
    });
    child.once('close', (code, signal) => {
      rejectReady(new Error(`PM3 fixture exited before ready: ${code} ${signal}\n${errors}`));
      resolve({ code, signal });
    });
  });
  try {
    await ready;
  } catch (error) {
    child.stdin.end();
    await closed;
    throw error;
  }
  return async () => {
    child.stdin.end('stop\n');
    const result = await closed;
    if (result.code !== 0) {
      throw new Error(`PM3 fixture cleanup failed: ${JSON.stringify(result)}\n${errors}`);
    }
    if (classicStop) {
      const cleanup = JSON.parse(await readFile(logfile + '.cleanup.json', 'utf8'));
      if (!cleanup.persistentStopPingConfirmed || !cleanup.ramRestored) {
        throw new Error(`Classic simulation did not confirm autonomous cleanup: ${JSON.stringify(cleanup)}`);
      }
    }
  };
}
