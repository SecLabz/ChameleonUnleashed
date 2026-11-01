import { ST25TB } from '../device/st25tb-protocol.mjs';
import { st25tbCounter } from '../documents/st25tb-plan.mjs';
import { $ } from './device-view.mjs';

function updateDialog(state) {
  $('st25-tear-block').textContent = String(state.block);
  $('st25-tear-current').textContent =
    state.flags & ST25TB.CURRENT_VALID ? state.current.toString(16).padStart(8, '0').toUpperCase() : '—';
  $('st25-tear-target').textContent = state.target.toString(16).padStart(8, '0').toUpperCase();
  $('st25-tear-attempts').textContent = String(state.attempts);
  $('st25-tear-timing').textContent = `${state.delay_us} µs`;
  $('st25-tear-duration').textContent = `${state.seconds} s`;
  $('st25-tear-progress').classList.toggle(
    'complete',
    state.state === ST25TB.DONE && state.outcome === ST25TB.SUCCESS
  );
}

export async function tearoffDialog(device, uid, item, signal) {
  const overlay = $('st25-tearoff');
  const cancel = $('st25-tear-cancel');

  const requestCancel = () => {
    if (cancel.disabled) {
      return;
    }

    cancel.disabled = true;
    cancel.textContent = 'Cancellation requested…';
    device.tearoffCancel().catch(() => {
      cancel.disabled = false;
      cancel.textContent = 'Retry cancel';
    });
  };

  cancel.disabled = true;
  cancel.textContent = 'Starting…';
  overlay.hidden = false;

  try {
    if (signal?.aborted) {
      throw new Error('Writing stopped before tearoff');
    }

    await device.tearoffStart(uid, item.block, st25tbCounter(item.after));
    cancel.disabled = false;
    cancel.textContent = 'Cancel';
    cancel.onclick = requestCancel;
    signal?.addEventListener('abort', requestCancel, { once: true });
    if (signal?.aborted) {
      requestCancel();
    }

    for (;;) {
      const state = await device.tearoffState();
      updateDialog(state);
      if (state.state === ST25TB.DONE) {
        const verified =
          state.outcome === ST25TB.SUCCESS &&
          state.flags & ST25TB.CLEANUP_COMPLETE &&
          state.current === state.target;
        if (!verified) {
          throw new Error(`Tearoff ended with outcome ${state.outcome}; reread before retrying`);
        }
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, ST25TB.POLL_MS));
    }
  } finally {
    signal?.removeEventListener('abort', requestCancel);
    overlay.hidden = true;
    cancel.onclick = null;
  }
}
