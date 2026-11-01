/**
 * Ported unchanged from web/app/src/transport/timer.ts.
 *
 * Timers that keep running when the tab is in the background. Chrome clamps main-thread timers in hidden tabs to
 * once a second (and later to once a minute), which would starve the session's keep-alives and the upload pacing;
 * timers inside a dedicated worker are not throttled that way. Falls back to setTimeout where workers are missing.
 */

import { type Sleep, timeoutSleep } from "./queue.ts";

const WORKER_SOURCE = `onmessage = (e) => setTimeout(() => postMessage(e.data.id), e.data.ms);`;

export function workerSleep(): Sleep {
  if (typeof Worker === "undefined" || typeof Blob === "undefined" || typeof URL.createObjectURL !== "function") {
    return timeoutSleep;
  }
  let worker: Worker;
  try {
    worker = new Worker(URL.createObjectURL(new Blob([WORKER_SOURCE], { type: "text/javascript" })));
  } catch {
    return timeoutSleep;
  }
  const pending = new Map<number, () => void>();
  let next = 1;
  worker.onmessage = (e: MessageEvent<number>) => {
    const resolve = pending.get(e.data);
    pending.delete(e.data);
    resolve?.();
  };
  return (ms) => new Promise<void>((resolve) => {
    const id = next++;
    pending.set(id, resolve);
    worker.postMessage({ id, ms });
  });
}
