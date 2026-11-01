/**
 * The risk notice. Shown after Enter, and before any page past the landing when this browser session hasn't
 * accepted it. Acceptance is kept for the session only (storage may be unavailable: then it's asked again).
 */

const KEY = "opfree.risk.accepted.v1";

export function hasAccepted(): boolean {
  try { return sessionStorage.getItem(KEY) === "1"; } catch { return false; }
}

function remember(): void {
  try { sessionStorage.setItem(KEY, "1"); } catch { /* private mode: ask again next time */ }
}

const NOTICE = `
  <form method="dialog" class="risk">
    <p class="label">Before you start</p>
    <h2 id="risk-title" class="risk__title">Read this before you change your OP&#8209;Z</h2>
    <div class="risk__body">
      <p>OP-FREE is experimental, unofficial software. It changes what runs on your OP-Z.</p>
      <ul class="risk__list">
        <li><b>Community engines run with no protection.</b> A faulty synth or effect can crash, freeze or restart the
          OP-Z, and an unsaved project can be lost. Engines are tested in a simulator, which can't catch everything.</li>
        <li><b>Your own samples.</b> Content mode deletes user samples that sit in no slot. The site checks for this,
          but keep copies of anything you care about.</li>
        <li><b>Firmware mods</b> (coming later) can cause data loss, permanent damage, or leave the OP-Z unable to start
          (“bricking”). Recovery is not guaranteed.</li>
        <li><b>Not affiliated with teenage engineering.</b> This project is independent and is not endorsed, sponsored
          or supported by teenage engineering. Using it may affect your warranty or support.</li>
        <li><b>We host no firmware and no keys.</b> When the firmware builder arrives, you bring your own firmware file
          and it stays in your browser.</li>
        <li><b>Provided as is, without warranty.</b> Back up your projects and samples in content mode before you
          install anything.</li>
      </ul>
    </div>
    <label class="risk__check">
      <input type="checkbox" name="accept" />
      <span>I have read this notice, understand the risks, and accept responsibility for anything that happens to my
        OP-Z or my data.</span>
    </label>
    <div class="risk__actions">
      <button type="button" class="btn btn--ghost" value="cancel">Back</button>
      <button type="submit" class="btn btn--primary" value="accept" disabled>Continue</button>
    </div>
  </form>`;

let open: Promise<boolean> | null = null;

/** Opens the notice (once, however often it's asked for). Resolves true when accepted, false when dismissed. */
export function askConsent(): Promise<boolean> {
  open ??= show().finally(() => { open = null; });
  return open;
}

function show(): Promise<boolean> {
  return new Promise((resolve) => {
    const dialog = document.createElement("dialog");
    dialog.className = "dialog";
    dialog.setAttribute("aria-labelledby", "risk-title");
    dialog.innerHTML = NOTICE;
    document.body.append(dialog);

    const box = dialog.querySelector<HTMLInputElement>("input[name=accept]")!;
    const go = dialog.querySelector<HTMLButtonElement>("button[value=accept]")!;
    const back = dialog.querySelector<HTMLButtonElement>("button[value=cancel]")!;
    box.addEventListener("change", () => { go.disabled = !box.checked; });
    back.addEventListener("click", () => dialog.close("cancel"));
    dialog.addEventListener("close", () => {
      const ok = dialog.returnValue === "accept" && box.checked;
      if (ok) remember();
      dialog.remove();
      resolve(ok);
    });
    dialog.showModal();
    box.focus();
  });
}
