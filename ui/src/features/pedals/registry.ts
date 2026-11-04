// SPDX-License-Identifier: AGPL-3.0-or-later
// The pedal registry as the engine declares it (engine/include/sedef/board/PedalList.inc, dumped
// to src/generated/pedals.json by `sedef-params --pedals`), and the board edits the UI makes.
// The rules mirror sedef::Board (engine/src/board/Board.cpp).
import generated from '../../generated/pedals.json';

export const CATEGORIES = ['Dynamics', 'Drive', 'Fuzz', 'Filter', 'Modulation', 'Pitch'] as const;
export type PedalCategory = (typeof CATEGORIES)[number];

export interface PedalMeta {
  /** Stable id, saved in sessions and presets. */
  id: string;
  name: string;
  category: PedalCategory;
  /** Default position: Add pedal puts a pedal after the last one at or below its rank. */
  rank: number;
  /** The pedal's on/off parameter. */
  enable: string;
}

const isCategory = (c: string): c is PedalCategory => (CATEGORIES as readonly string[]).includes(c);

export const PEDALS: readonly PedalMeta[] = generated.pedals.map((p) => ({
  ...p,
  category: isCategory(p.category) ? p.category : 'Dynamics',
}));

const BY_ID = new Map(PEDALS.map((p) => [p.id, p]));

export function pedalMeta(id: string): PedalMeta | undefined {
  return BY_ID.get(id);
}

export function isPedalId(id: unknown): id is string {
  return typeof id === 'string' && BY_ID.has(id);
}

export function pedalName(id: string): string {
  return BY_ID.get(id)?.name ?? id;
}

/** The pedals 0.3.0 had, in its fixed order. */
export const LEGACY_BOARD: readonly string[] = generated.legacy.filter(isPedalId);

/** Known ids, each once, in the order given: what the plugin keeps of a board. */
export function sanitiseBoard(ids: unknown): string[] {
  if (!Array.isArray(ids)) return [...LEGACY_BOARD];
  const out: string[] = [];
  for (const id of ids) if (isPedalId(id) && !out.includes(id)) out.push(id);
  return out;
}

/** Adds a pedal at its default position (no change when it is on the board already). */
export function addPedal(board: readonly string[], id: string): string[] {
  const meta = pedalMeta(id);
  if (!meta || board.includes(id)) return [...board];
  let at = 0;
  board.forEach((other, i) => {
    if ((pedalMeta(other)?.rank ?? 0) <= meta.rank) at = i + 1;
  });
  return [...board.slice(0, at), id, ...board.slice(at)];
}

export function removePedal(board: readonly string[], id: string): string[] {
  return board.filter((p) => p !== id);
}

/** Moves a pedal so it ends up at `index` (clamped). */
export function movePedal(board: readonly string[], id: string, index: number): string[] {
  if (!board.includes(id)) return [...board];
  const rest = removePedal(board, id);
  const at = Math.max(0, Math.min(rest.length, Math.round(index)));
  return [...rest.slice(0, at), id, ...rest.slice(at)];
}

/**
 * The board of a preset or session saved without one: the legacy board, plus every other pedal
 * it switches on, at that pedal's default position (sedef::Board::legacyFor).
 */
export function legacyBoardFor(isOn: (enableParam: string) => boolean): string[] {
  let board = [...LEGACY_BOARD];
  for (const p of PEDALS) if (isOn(p.enable)) board = addPedal(board, p.id);
  return board;
}

/**
 * Only the pedals switched on, each at its default position (sedef::Board::switchedOn): Init's
 * board and a fresh instance's (the Gate alone), and what each factory preset carries.
 */
export function switchedOnBoardFor(isOn: (enableParam: string) => boolean): string[] {
  let board: string[] = [];
  for (const p of PEDALS) if (isOn(p.enable)) board = addPedal(board, p.id);
  return board;
}

export function sameBoard(a: readonly string[], b: readonly string[]): boolean {
  return a.length === b.length && a.every((id, i) => b[i] === id);
}
