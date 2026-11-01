import { hfDocument } from '../documents/hf-image.mjs';
import { Device, hex } from './device.mjs';
import { readHf } from './hf-reader.mjs';
import { ST25TB, st25tbIdentity } from './st25tb-protocol.mjs';
import { planSt25tb } from '../documents/st25tb-plan.mjs';

const same = (actual, expected) => JSON.stringify(actual) === JSON.stringify(expected);

async function physicalCapture(device, uid) {
  const card = st25tbIdentity(await device.command(ST25TB.INFO));
  if (!card || card.blocks !== ST25TB.BLOCKS || hex(card.uid) !== hex(uid)) {
    throw new Error('Physical tag identity changed');
  }

  const captured = hfDocument(card);
  await readHf(device, captured, () => {});
  if (!captured.capture.complete || !captured.capture.system) {
    throw new Error('A complete physical reread is required before writing');
  }
  return captured.capture;
}

async function verifiedWrite(device, uid, item) {
  const payload = [...uid, item.block, ...item.before, ...item.after];
  const response = await device.writeCommand(ST25TB.WRITE, payload);
  if (!same([...response], item.after)) {
    throw new Error(`Block ${item.block}: write verification failed`);
  }

  const readback = await device.command(ST25TB.READ, [...uid, item.block]);
  if (!same([...readback], item.after)) {
    throw new Error(`Block ${item.block}: independent readback failed`);
  }
}

export async function writeSt25tb(documentState, signal, progress, { confirmPlan, confirmOtp, tearoff }) {
  const device = new Device();
  let error;

  try {
    await device.reserve();
    const uid = documentState.card.uid;
    const before = await physicalCapture(device, uid);
    const plan = planSt25tb(documentState, before);
    if (!plan.writes.length && !plan.tearoffs.length && !plan.otp) {
      progress('The physical tag already matches this document');
      await device.end();
      return false;
    }

    const confirmed = await confirmPlan(plan);
    if (!confirmed) {
      await device.end();
      return;
    }

    const current = await physicalCapture(device, uid);
    if (!same(current.memory, before.memory) || !same(current.system, before.system)) {
      throw new Error('The tag changed during review; no write was sent');
    }

    for (const item of plan.writes) {
      if (signal?.aborted) {
        throw new Error('Writing stopped before the next block');
      }
      progress(`Writing block ${item.block}…`);
      await verifiedWrite(device, uid, item);
    }

    for (const item of plan.tearoffs) {
      if (signal?.aborted) {
        throw new Error('Writing stopped before tearoff');
      }
      progress(`Tearoff on block ${item.block}…`);
      await tearoff(device, uid, item, signal);
      const readback = await device.command(ST25TB.READ, [...uid, item.block]);
      if (!same([...readback], item.after)) {
        throw new Error(`Tearoff block ${item.block}: final readback differs`);
      }
    }

    if (plan.otp) {
      if (signal?.aborted) {
        throw new Error('Writing stopped before sysOTP');
      }

      if (!(await confirmOtp(plan.otp))) {
        throw new Error('sysOTP write cancelled');
      }

      if (signal?.aborted) {
        throw new Error('Writing stopped before sysOTP');
      }
      await verifiedWrite(device, uid, plan.otp);
    }

    progress('All requested changes were written; rereading the tag…');
  } catch (caught) {
    error = caught;
  }

  try {
    if (device.session) {
      const actual = await physicalCapture(device, documentState.card.uid);
      documentState.capture = actual;
      documentState.memory = actual.memory.map((block) => block && [...block]);
      documentState.card.system = actual.system;
      documentState.card.chip_id = actual.chip_id;

      progress(error ? `${error.message} · physical tag reread` : 'ST25TB write and full reread verified');
    }
  } catch (readError) {
    error = new Error(
      `${error?.message || 'Write outcome unknown'}; final reread failed: ${readError.message}`
    );
  } finally {
    try {
      await device.end();
    } catch (cleanupError) {
      error = new Error(`${error?.message || 'Write completed'}; cleanup failed: ${cleanupError.message}`);
    }
  }

  if (error) {
    throw error;
  }
  return true;
}
