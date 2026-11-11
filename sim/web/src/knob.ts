/**
 * A parameter knob for one byte (0-255): drag up/down, wheel, arrow keys (shift for steps of 16), double-click to
 * reset. An ARIA slider, so it works with a keyboard and a screen reader.
 */

const SWEEP = 270;            // degrees from 0 to 255

export interface Knob {
  el: HTMLElement;
  set(value: number): void;
  get(): number;
  rename(label: string): void;
  /** The learned MIDI CC shown under the knob (null: none). */
  setCc(text: string | null): void;
  /** Waiting for a CC in learn mode. */
  setArmed(armed: boolean): void;
}

function arc(value: number): string {
  const start = -SWEEP / 2, end = start + (SWEEP * value) / 255;
  const p = (deg: number) => {
    const r = ((deg - 90) * Math.PI) / 180;
    return `${(24 + 18 * Math.cos(r)).toFixed(2)} ${(24 + 18 * Math.sin(r)).toFixed(2)}`;
  };
  if (value <= 0) return "";
  return `M ${p(start)} A 18 18 0 ${end - start > 180 ? 1 : 0} 1 ${p(end)}`;
}

export function knob(label: string, index: number, initial: number, reset: number, onChange: (v: number) => void): Knob {
  let value = initial;
  const el = document.createElement("div");
  el.className = "knob";
  el.innerHTML = `
    <div class="knob-dial" role="slider" tabindex="0" aria-valuemin="0" aria-valuemax="255">
      <svg viewBox="0 0 48 48" aria-hidden="true">
        <path class="knob-track" d="${arc(255)}" />
        <path class="knob-value" />
        <line class="knob-pointer" x1="24" y1="24" x2="24" y2="9" />
      </svg>
    </div>
    <div class="knob-label"></div>
    <div class="knob-number"></div>
    <div class="knob-cc" hidden></div>`;
  const dial = el.querySelector<HTMLElement>(".knob-dial")!;
  const valuePath = el.querySelector<SVGPathElement>(".knob-value")!;
  const pointer = el.querySelector<SVGLineElement>(".knob-pointer")!;
  const labelEl = el.querySelector<HTMLElement>(".knob-label")!;
  const numberEl = el.querySelector<HTMLElement>(".knob-number")!;
  const ccEl = el.querySelector<HTMLElement>(".knob-cc")!;
  dial.dataset.param = String(index);

  const draw = () => {
    valuePath.setAttribute("d", arc(value));
    pointer.setAttribute("transform", `rotate(${-SWEEP / 2 + (SWEEP * value) / 255} 24 24)`);
    numberEl.textContent = String(value);
    dial.setAttribute("aria-valuenow", String(value));
  };
  const change = (v: number) => {
    v = Math.max(0, Math.min(255, Math.round(v)));
    if (v === value) return;
    value = v;
    draw();
    onChange(v);
  };
  const rename = (text: string) => {
    labelEl.textContent = text;
    dial.setAttribute("aria-label", text);
  };
  rename(label);

  let dragY = 0, dragValue = 0;
  dial.addEventListener("pointerdown", (e) => {
    dial.setPointerCapture(e.pointerId);
    dragY = e.clientY;
    dragValue = value;
    dial.focus();
  });
  dial.addEventListener("pointermove", (e) => {
    if (!dial.hasPointerCapture(e.pointerId)) return;
    change(dragValue + ((dragY - e.clientY) * (e.shiftKey ? 0.25 : 1.5)));
  });
  dial.addEventListener("wheel", (e) => {
    e.preventDefault();
    change(value - Math.sign(e.deltaY) * (e.shiftKey ? 1 : 4));
  }, { passive: false });
  dial.addEventListener("keydown", (e) => {
    const step = e.shiftKey ? 16 : 1;
    if (e.key === "ArrowUp" || e.key === "ArrowRight") change(value + step);
    else if (e.key === "ArrowDown" || e.key === "ArrowLeft") change(value - step);
    else if (e.key === "Home") change(0);
    else if (e.key === "End") change(255);
    else return;
    e.preventDefault();
    e.stopPropagation();
  });
  dial.addEventListener("dblclick", () => change(reset));
  draw();

  return {
    el,
    set(v: number) {
      value = v;
      draw();
    },
    get: () => value,
    rename,
    setCc(text: string | null) {
      ccEl.hidden = !text;
      ccEl.textContent = text ?? "";
    },
    setArmed(armed: boolean) {
      el.classList.toggle("armed", armed);
    },
  };
}
