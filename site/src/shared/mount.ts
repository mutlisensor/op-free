/** What a page module (the lab, the loader) gives back when it is mounted: the router calls destroy on leaving. */
export interface Mounted {
  destroy(): void;
}

/**
 * The synths page's two halves talk through DOM events on `document`, so neither imports the other:
 * - `opfree:audition` (detail: { dir }) — the loader asks the lab to load that engine.
 * - `opfree:pick` (detail: { dir, picked }) — the lab's "add to my OP-Z" toggled an engine in the loader.
 */
export type AuditionEvent = CustomEvent<{ dir: string }>;
export type PickEvent = CustomEvent<{ dir: string; picked: boolean }>;
