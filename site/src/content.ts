/** Links and copy shared by the pages. The repository name is set once here. */

export const REPO = "https://github.com/mutlisensor/op-free";
const tree = (path: string) => `${REPO}/tree/main/${path}`;
const blob = (path: string) => `${REPO}/blob/main/${path}`;

export const LINKS = {
  repo: REPO,
  sdk: tree("sdk"),
  quickstart: blob("sdk/docs/QUICKSTART.md"),
  research: tree("research"),
  engines: tree("engines"),
  guides: tree("guides"),
  useTheSite: blob("guides/using-op-free.md"),
  extractFirmware: blob("guides/extract-your-firmware.md"),
  firmwareExplained: blob("guides/how-the-firmware-works.md"),
  submitEngine: blob("guides/submit-an-engine.md"),
  proposeMod: blob("guides/propose-a-firmware-mod.md"),
  roadmap: blob("ROADMAP.md"),
  issues: `${REPO}/issues`,
};

/** The steps a visitor goes through to get sounds onto their OP-Z. Shown on the home page and the synths page. */
export const STEPS: { title: string; body: string; where: string }[] = [
  { title: "Try", where: "in the browser", body: "Play every synth and effect here first, with its presets, from the on-screen keys or a MIDI keyboard. Nothing touches your OP-Z." },
  { title: "Pick", where: "in the browser", body: "Choose the sounds for each track. Eight are ticked for you, leaving two slots free for your own samples. Switch between community engines and the original OP-Z sounds." },
  { title: "Connect & back up", where: "USB · Chrome or Edge", body: "Close teenage engineering's app, plug in the OP-Z and connect. The site backs up your OP-Z's settings before it changes anything." },
  { title: "Install", where: "USB MIDI", body: "The engines go in over USB, the same route the official app uses. Every write is shown first, confirmed by you, then read back and checked." },
  { title: "Content mode", where: "hold track + power on", body: "The OP-Z shows up as a disk. The site writes your presets into it so they survive a restart, and puts each sound in its slot." },
  { title: "Eject & play", where: "on the OP-Z", body: "Eject, the OP-Z restarts with your sounds. Want it back the way it was? Restore the backup from step three at any time." },
];

/** Firmware work on the list: research first, none of it built yet. */
export const FIRMWARE_PLANS: { title: string; body: string }[] = [
  { title: "64-step sequencer", body: "Four pages of 16 steps per pattern." },
  { title: "Button debounce", body: "Stop the keys that double-trigger." },
  { title: "Bluetooth audio", body: "What the radio and the firmware can actually do." },
  { title: "Arpeggiator & player styles", body: "New note styles for the arp and player tracks." },
  { title: "Modules", body: "Updates for the line module and DMX out." },
];
