// SPDX-License-Identifier: AGPL-3.0-or-later
// The pedal registry as the engine declares it (engine/include/sedef/board/PedalList.inc, dumped
// to src/generated/pedals.json by `sedef-params --pedals`), and the board edits the UI makes.
// The rules mirror sedef::Board and sedef::BoardReader (engine/src/board/Board.cpp,
// BoardText.cpp; docs/board.md).
//
// A board is a list of tokens in signal order: a pedal's id for its first copy ("kelp"), the id
// and "#2" for its second ("kelp#2"), and "amp" before the pedals after the amp. "amp" is there
// only when a pedal stands after it, so a board with nothing after the amp is spelt as before.
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
  /** Its second copy's on/off parameter. */
  enable2: string;
  /** It may stand after the amp (modulation). */
  afterAmp: boolean;
  /** It has latency (a drive or fuzz): the board holds LATENCY_SLOTS of them at most. */
  latency: boolean;
}

const isCategory = (c: string): c is PedalCategory => (CATEGORIES as readonly string[]).includes(c);

export const PEDALS: readonly PedalMeta[] = generated.pedals.map((p) => ({
  ...p,
  category: isCategory(p.category) ? p.category : 'Dynamics',
}));

/** Copies of one pedal a board holds. */
export const COPIES: number = generated.copies;
/** Drives and fuzzes a board holds in all. */
export const LATENCY_SLOTS: number = generated.latencySlots;
/** Where the amp is, in a board's tokens. */
export const AMP = 'amp';

const BY_ID = new Map(PEDALS.map((p) => [p.id, p]));

/** A token's pedal and copy (0 or 1), or null for "amp" and unknown ids. */
export function parseToken(token: unknown): { id: string; copy: number } | null {
  if (typeof token !== 'string') return null;
  const hash = token.indexOf('#');
  if (hash < 0) return BY_ID.has(token) ? { id: token, copy: 0 } : null;
  const id = token.slice(0, hash);
  return token.slice(hash + 1) === '2' && BY_ID.has(id) ? { id, copy: 1 } : null;
}

export function tokenOf(id: string, copy: number): string {
  return copy > 0 ? `${id}#${copy + 1}` : id;
}

/** The pedal a token names (its first copy's id), or the token itself. */
export function baseId(token: string): string {
  return parseToken(token)?.id ?? token;
}

/** Which copy a token is: 1 or 2. */
export function copyNumber(token: string): number {
  return (parseToken(token)?.copy ?? 0) + 1;
}

export function pedalMeta(token: string): PedalMeta | undefined {
  return BY_ID.get(baseId(token));
}

export function isPedalId(id: unknown): id is string {
  return parseToken(id) !== null;
}

/** "Kelp" for a first copy, "Kelp 2" for a second: as the host names their parameters. */
export function pedalName(token: string): string {
  const parsed = parseToken(token);
  if (!parsed) return token === AMP ? 'the amp' : token;
  const name = BY_ID.get(parsed.id)?.name ?? parsed.id;
  return parsed.copy > 0 ? `${name} ${parsed.copy + 1}` : name;
}

/** A copy's on/off parameter. */
export function enableOf(token: string): string {
  const parsed = parseToken(token);
  const meta = parsed && BY_ID.get(parsed.id);
  if (!parsed || !meta) return '';
  return parsed.copy > 0 ? meta.enable2 : meta.enable;
}

/**
 * A parameter of a pedal's copy: the first copy's id for copy 1, the pedal id followed by 2 for
 * copy 2 ("kelpDrive" -> "kelp2Drive"; docs/board.md §7).
 */
export function copyParam(param: string, pedal: string, copy: number): string {
  if (copy <= 1 || !param.startsWith(pedal)) return param;
  return `${pedal}${copy}${param.slice(pedal.length)}`;
}

/** The pedals 0.3.0 had, in its fixed order. */
export const LEGACY_BOARD: readonly string[] = generated.legacy.filter((id) => BY_ID.has(id));

// -------------------------------------------------------------------------------------------------
// The board's two sides

export type Side = 'pre' | 'post';

/** The pedals in front of the amp and after it. */
export function sides(board: readonly string[]): { pre: string[]; post: string[] } {
  const at = board.indexOf(AMP);
  return at < 0
    ? { pre: [...board], post: [] }
    : { pre: board.slice(0, at), post: board.slice(at + 1) };
}

/** The tokens of two sides: "amp" only when a pedal stands after it. */
export function joinSides(pre: readonly string[], post: readonly string[]): string[] {
  return post.length > 0 ? [...pre, AMP, ...post] : [...pre];
}

/** The board with "amp" in it whether or not a pedal stands after it: what the page draws. */
export function withAmp(board: readonly string[]): string[] {
  const { pre, post } = sides(board);
  return [...pre, AMP, ...post];
}

export function sideOf(board: readonly string[], token: string): Side | null {
  const { pre, post } = sides(board);
  return pre.includes(token) ? 'pre' : post.includes(token) ? 'post' : null;
}

/** Drives and fuzzes on the board. */
export function latencyCount(board: readonly string[]): number {
  return board.filter((t) => t !== AMP && pedalMeta(t)?.latency).length;
}

/** Whether `token` may stand on `side` of the board (moving it there, if it is on already). */
export function canPlace(board: readonly string[], token: string, side: Side): boolean {
  const meta = parseToken(token) ? pedalMeta(token) : undefined;
  if (!meta) return false;
  if (side === 'post' && !meta.afterAmp) return false;
  return board.includes(token) || !meta.latency || latencyCount(board) < LATENCY_SLOTS;
}

/** Why Add can't put another copy of `id` on the board, or null when it can. */
export function addBlocked(board: readonly string[], id: string): 'both' | 'drives' | null {
  if (Array.from({ length: COPIES }, (_, c) => tokenOf(id, c)).every((t) => board.includes(t)))
    return 'both';
  return pedalMeta(id)?.latency && latencyCount(board) >= LATENCY_SLOTS ? 'drives' : null;
}

/** The copy Add would put on next: the lowest one not on the board, if the board takes it. */
export function nextCopy(board: readonly string[], id: string, side: Side = 'pre'): string | null {
  if (!BY_ID.has(id)) return null;
  for (let c = 0; c < COPIES; c += 1) {
    const token = tokenOf(id, c);
    if (!board.includes(token)) return canPlace(board, token, side) ? token : null;
  }
  return null;
}

/** Puts `token` on `side` at `index` (clamped), moving it there if it is on the board already. */
export function placePedal(
  board: readonly string[],
  token: string,
  side: Side,
  index: number,
): string[] {
  if (!canPlace(board, token, side)) return [...board];
  const { pre, post } = sides(removePedal(board, token));
  const list = side === 'pre' ? pre : post;
  const at = Math.max(0, Math.min(list.length, Math.round(index)));
  list.splice(at, 0, token);
  return joinSides(pre, post);
}

/** Where a pedal goes by default on one side: after the last pedal there at or below its rank. */
function rankIndex(list: readonly string[], id: string): number {
  const rank = BY_ID.get(id)?.rank ?? 0;
  let at = 0;
  list.forEach((other, i) => {
    if ((pedalMeta(other)?.rank ?? 0) <= rank) at = i + 1;
  });
  return at;
}

/**
 * Add pedal: the next copy, right after the other copy when that is on the board (on its side),
 * else in front of the amp at its default position. No change when the board can't take one.
 */
export function addPedal(board: readonly string[], id: string): string[] {
  const token = nextCopy(board, id);
  if (!token) return [...board];
  for (let c = 0; c < COPIES; c += 1) {
    const other = tokenOf(id, c);
    const side = sideOf(board, other);
    if (side) return placePedal(board, token, side, sides(board)[side].indexOf(other) + 1);
  }
  return placePedal(board, token, 'pre', rankIndex(sides(board).pre, id));
}

/** The next copy at the end of the side after the amp. */
export function addAfterAmp(board: readonly string[], id: string): string[] {
  const token = nextCopy(board, id, 'post');
  return token ? placePedal(board, token, 'post', sides(board).post.length) : [...board];
}

export function removePedal(board: readonly string[], token: string): string[] {
  const { pre, post } = sides(board);
  return joinSides(
    pre.filter((p) => p !== token),
    post.filter((p) => p !== token),
  );
}

/**
 * Moves a pedal so it ends up at `index` in the board as drawn (withAmp) without it, clamped. A
 * pedal that can't stand after the amp stops in front of it.
 */
export function movePedal(board: readonly string[], token: string, index: number): string[] {
  if (token === AMP || !board.includes(token)) return [...board];
  const rest = withAmp(board).filter((t) => t !== token);
  const amp = rest.indexOf(AMP);
  let at = Math.max(0, Math.min(rest.length, Math.round(index)));
  if (at > amp && !pedalMeta(token)?.afterAmp) at = amp;
  return at <= amp
    ? placePedal(board, token, 'pre', at)
    : placePedal(board, token, 'post', at - amp - 1);
}

/**
 * What the plugin keeps of a list of tokens (sedef::BoardReader): known pedals once each, the side
 * switching at the first "amp", only what a side takes, at most LATENCY_SLOTS drives and fuzzes.
 */
export function sanitiseBoard(ids: unknown): string[] {
  if (!Array.isArray(ids)) return [...LEGACY_BOARD];
  const pre: string[] = [];
  const post: string[] = [];
  let side: Side = 'pre';
  for (const id of ids) {
    if (id === AMP) {
      side = 'post';
      continue;
    }
    if (!isPedalId(id) || pre.includes(id) || post.includes(id)) continue;
    if (canPlace(joinSides(pre, post), id, side)) (side === 'pre' ? pre : post).push(id);
  }
  return joinSides(pre, post);
}

/**
 * The board of a preset or session saved without one: the legacy board, plus every other pedal
 * it switches on, at that pedal's default position (sedef::Board::legacyFor).
 */
export function legacyBoardFor(isOn: (enableParam: string) => boolean): string[] {
  let board = [...LEGACY_BOARD];
  for (const p of PEDALS)
    if (isOn(p.enable) && !board.includes(p.id)) board = addPedal(board, p.id);
  return board;
}

/**
 * Only the pedals switched on, each first copy at its default position and a second copy that is
 * on right after its first (sedef::Board::switchedOn): Init's board and a fresh instance's (the
 * Gate alone), and what each factory preset carries.
 */
export function switchedOnBoardFor(isOn: (enableParam: string) => boolean): string[] {
  let board: string[] = [];
  for (const p of PEDALS)
    if (isOn(p.enable)) board = placePedal(board, p.id, 'pre', rankIndex(board, p.id));
  for (const p of PEDALS) {
    const second = tokenOf(p.id, 1);
    if (!isOn(p.enable2) || !canPlace(board, second, 'pre')) continue;
    const first = board.indexOf(p.id);
    board = placePedal(board, second, 'pre', first >= 0 ? first + 1 : rankIndex(board, p.id));
  }
  return board;
}

/** Where a pedal sits, for what is announced after it moved or joined: "2 of 3 after the amp, after Gyre". */
export function positionText(board: readonly string[], token: string): string {
  const side = sideOf(board, token);
  if (!side) return '';
  const list = sides(board)[side];
  const at = list.indexOf(token);
  const where = at === 0 ? 'first' : `after ${pedalName(list[at - 1] ?? '')}`;
  const zone = side === 'pre' ? 'in front of the amp' : 'after the amp';
  return `${at + 1} of ${list.length} ${zone}, ${where}`;
}

export function sameBoard(a: readonly string[], b: readonly string[]): boolean {
  return a.length === b.length && a.every((id, i) => b[i] === id);
}
