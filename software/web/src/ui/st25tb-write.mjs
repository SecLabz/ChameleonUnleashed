import { writeSt25tb as writeToDevice } from '../device/st25tb-write.mjs';
import { ask } from './dialogs.mjs';
import { tearoffDialog } from './st25tb-dialog.mjs';

function wordHex(bytes) {
  if (!bytes) {
    return '????????';
  }
  return [...bytes]
    .reverse()
    .map((byte) => byte.toString(16).padStart(2, '0'))
    .join('')
    .toUpperCase();
}

function describe(plan) {
  const lines = [];
  for (const item of plan.writes) {
    lines.push(
      `Block ${String(item.block).padStart(2, '0')}  ${wordHex(item.before)} → ${wordHex(item.after)}`
    );
  }

  for (const item of plan.tearoffs) {
    lines.push(`Tearoff ${item.block}  ${wordHex(item.before)} → ${wordHex(item.after)}`);
  }

  if (plan.otp) {
    lines.push(`sysOTP  ${wordHex(plan.otp.before)} → ${wordHex(plan.otp.after)}`);
  }
  return lines.join('\n');
}

export function writeSt25tb(documentState, signal, progress) {
  return writeToDevice(documentState, signal, progress, {
    confirmPlan: (plan) =>
      ask({
        title: 'Write ST25TB changes?',
        message: describe(plan),
        confirmLabel: 'Write changes'
      }),
    async confirmOtp(item) {
      const answer = await ask({
        title: 'Write irreversible sysOTP',
        message: `${wordHex(item.before)} → ${wordHex(item.after)}\nType WRITE OTP to continue.`,
        initial: '',
        confirmLabel: 'Write sysOTP'
      });
      return answer === 'WRITE OTP';
    },
    tearoff: tearoffDialog
  });
}
