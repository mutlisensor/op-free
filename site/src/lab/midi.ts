/**
 * Web MIDI note input for the lab: one chosen input port, note on/off with velocity and pitch bend. Asked for without
 * SysEx (no permission prompt for device access in Chrome, and nothing is ever sent: the lab opens no output).
 */

export interface MidiHandlers {
  note(on: boolean, note: number, velocity: number): void;
  bend(cents: number): void;
  /** the list of inputs changed (a device was plugged in or out) */
  ports(inputs: Array<{ id: string; name: string }>): void;
}

export class MidiIn {
  private access: MIDIAccess | null = null;
  private current: MIDIInput | null = null;
  private readonly h: MidiHandlers;

  constructor(handlers: MidiHandlers) {
    this.h = handlers;
  }

  static get supported(): boolean {
    return typeof navigator !== "undefined" && "requestMIDIAccess" in navigator;
  }

  /** Ask for access; resolves with an error message, or null when it worked. */
  async connect(): Promise<string | null> {
    if (!MidiIn.supported) return "This browser has no Web MIDI. Chrome and Edge have it.";
    try {
      this.access = await navigator.requestMIDIAccess({ sysex: false });
    } catch (e) {
      return (e as Error).message || "MIDI access was refused.";
    }
    this.access.onstatechange = () => this.list();
    this.list();
    return null;
  }

  private inputs(): MIDIInput[] {
    return this.access ? [...this.access.inputs.values()] : [];
  }

  private list(): void {
    const inputs = this.inputs();
    this.h.ports(inputs.map((i) => ({ id: i.id, name: i.name ?? "MIDI input" })));
    if (this.current && !inputs.some((i) => i.id === this.current!.id)) this.select(null);
  }

  /** Listen to one input (null: none). */
  select(id: string | null): void {
    if (this.current) this.current.onmidimessage = null;
    this.current = this.inputs().find((i) => i.id === id) ?? null;
    if (!this.current) return;
    this.current.onmidimessage = (m) => {
      const d = m.data;
      if (!d || d.length < 2) return;
      const status = d[0] & 0xf0;
      if (status === 0x90 && d.length >= 3) this.h.note(d[2] > 0, d[1], d[2]);
      else if (status === 0x80) this.h.note(false, d[1], 0);
      else if (status === 0xe0 && d.length >= 3) this.h.bend(Math.round(((((d[2] << 7) | d[1]) - 8192) / 8192) * 200));
    };
  }

  destroy(): void {
    this.select(null);
    if (this.access) this.access.onstatechange = null;
    this.access = null;
  }
}
