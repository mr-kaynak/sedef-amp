// SPDX-License-Identifier: AGPL-3.0-or-later
// The preset browser's model, as pure functions: which presets a library view holds, the
// order and grouping by category, search, and the signal-chain summary of a row.
import type { PostBlock, PresetBlock, PresetInfo } from '../../bridge/protocol';
import { AMP, isPedalId, pedalName } from '../pedals/registry';
import { AMP_NAMES, bundledSchema } from '../../params/schema';

export type Library = 'all' | 'favorites' | 'recent' | 'user';

export interface BrowseFilter {
  library: Library;
  query: string;
}

export const DEFAULT_FILTER: BrowseFilter = { library: 'all', query: '' };

export const LIBRARIES: readonly { id: Library; label: string }[] = [
  { id: 'all', label: 'All' },
  { id: 'favorites', label: 'Favourites' },
  { id: 'recent', label: 'Recent' },
  { id: 'user', label: 'User' },
];

/** The factory categories in the order a player reaches for them, from clean to ambient. */
export const CATEGORY_ORDER: readonly string[] = [
  'Clean',
  'Crunch',
  'High Gain',
  'Lead',
  'Ambient',
];

const natural = (a: string, b: string) =>
  a.localeCompare(b, 'en', { numeric: true, sensitivity: 'base' });

/**
 * Presets in library order (PresetManager::scan): the factory categories from clean to ambient,
 * then the user's own folders by name, then presets without a folder; inside a category the
 * factory presets first, each by name. Previous and next step in this order too.
 */
export function comparePresets(a: PresetInfo, b: PresetInfo): number {
  const rank = (folder: string) => {
    if (!folder) return CATEGORY_ORDER.length + 1;
    const i = CATEGORY_ORDER.indexOf(folder);
    return i < 0 ? CATEGORY_ORDER.length : i;
  };
  const source = (p: PresetInfo) => (p.category === 'Factory' ? 0 : 1);
  return (
    rank(a.folder) - rank(b.folder) ||
    natural(a.folder, b.folder) ||
    source(a) - source(b) ||
    natural(a.name, b.name)
  );
}

export function inLibrary(p: PresetInfo, library: Library, recent: readonly string[]): boolean {
  switch (library) {
    case 'all':
      return true;
    case 'favorites':
      return p.favorite;
    case 'recent':
      return recent.includes(p.id);
    case 'user':
      return p.category === 'User';
  }
}

export function libraryCounts(
  items: readonly PresetInfo[],
  recent: readonly string[],
): Record<Library, number> {
  const count = (library: Library) => items.filter((p) => inLibrary(p, library, recent)).length;
  return {
    all: items.length,
    favorites: count('favorites'),
    recent: count('recent'),
    user: count('user'),
  };
}

export interface Facet {
  name: string;
  count: number;
}

/** Tags carried by these presets, most common first. */
export function tagFacets(items: readonly PresetInfo[]): Facet[] {
  const counts = new Map<string, number>();
  for (const p of items) for (const t of p.tags) counts.set(t, (counts.get(t) ?? 0) + 1);
  return [...counts.entries()]
    .map(([name, count]) => ({ name, count }))
    .sort((a, b) => b.count - a.count || natural(a.name, b.name));
}

// Signal chain ------------------------------------------------------------------------------------

const POST_LABELS: Record<PostBlock, string> = {
  eq: 'EQ',
  doubler: 'Doubler',
  drift: 'Drift',
  fathom: 'Fathom',
};

/** A block's name: a pedal's from the registry, else the post block's. */
export function blockLabel(b: PresetBlock): string {
  return isPedalId(b) ? pedalName(b) : (POST_LABELS[b as PostBlock] ?? b);
}

/** Cab names in cabModel order ("1x12 Open Back"…), and their short form ("1x12"). */
export const CAB_NAMES: readonly string[] = bundledSchema.find((s) => s.id === 'cabModel')
  ?.choices ?? ['1x12', '2x12', '4x12'];
export const CAB_LABELS: readonly string[] = CAB_NAMES.map((c) => c.split(' ')[0] ?? c);

export interface ChainPart {
  kind: 'block' | 'amp';
  label: string;
  /** The cab, for the amp part. */
  detail?: string;
}

export function ampName(p: Pick<PresetInfo, 'amp'>): string {
  return AMP_NAMES[p.amp] ?? 'Reef';
}

/**
 * The switched-on chain in signal order: the pedals in front of the amp in their board's order,
 * then amp and cab, the pedals after the amp and the post effects. The gate is left out: it is on
 * in almost every preset and says nothing about the sound.
 */
export function chainParts(p: Pick<PresetInfo, 'amp' | 'cab' | 'blocks'>): ChainPart[] {
  const block = (b: PresetBlock): ChainPart => ({ kind: 'block', label: blockLabel(b) });
  const amp = p.blocks.indexOf(AMP);
  const front = amp < 0 ? p.blocks : p.blocks.slice(0, amp);
  const back = amp < 0 ? [] : p.blocks.slice(amp + 1);
  const pre = front.filter((b) => isPedalId(b) && b !== 'gate');
  const post = [...back.filter(isPedalId), ...p.blocks.filter((b) => b !== AMP && !isPedalId(b))];
  return [
    ...pre.map(block),
    { kind: 'amp', label: ampName(p), detail: CAB_LABELS[p.cab] ?? '' },
    ...post.map(block),
  ];
}

/** "Tide → Pearl → Reef 2x12 → Drift → Fathom" */
export function chainText(p: Pick<PresetInfo, 'amp' | 'cab' | 'blocks'>): string {
  return chainParts(p)
    .map((part) => (part.detail ? `${part.label} ${part.detail}` : part.label))
    .join(' → ');
}

// Search and sort -----------------------------------------------------------------------------------

function haystack(p: PresetInfo): string {
  return [
    p.name,
    p.folder,
    p.author,
    p.description,
    ...p.tags,
    ampName(p),
    CAB_LABELS[p.cab] ?? '',
    ...p.blocks.map(blockLabel),
  ]
    .join(' ')
    .toLowerCase();
}

/** Every word of the query must appear in the name, category, tags, description, amp or chain. */
export function matchesQuery(p: PresetInfo, query: string): boolean {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  if (!words.length) return true;
  const text = haystack(p);
  return words.every((w) => text.includes(w));
}

/**
 * The presets a filter shows, in display order: library order, or newest first in Recent.
 */
export function browse(
  items: readonly PresetInfo[],
  filter: BrowseFilter,
  recent: readonly string[],
): PresetInfo[] {
  const shown = items.filter(
    (p) => inLibrary(p, filter.library, recent) && matchesQuery(p, filter.query),
  );
  if (filter.library !== 'recent') return shown.sort(comparePresets);
  return shown.sort((a, b) => recent.indexOf(a.id) - recent.indexOf(b.id));
}

export interface PresetGroup {
  /** The category; '' for presets without one. */
  name: string;
  items: PresetInfo[];
}

/** Consecutive presets of one category, as the list shows them under a header. */
export function groupByCategory(items: readonly PresetInfo[]): PresetGroup[] {
  const groups: PresetGroup[] = [];
  for (const p of items) {
    const last = groups[groups.length - 1];
    if (last && last.name === p.folder) last.items.push(p);
    else groups.push({ name: p.folder, items: [p] });
  }
  return groups;
}

/** Every folder in use, for the save form's category picker. */
export function allCategories(items: readonly PresetInfo[]): string[] {
  return [...new Set(items.map((p) => p.folder).filter(Boolean))].sort(natural);
}

/** Every tag in use, most common first, for suggestions. */
export function allTags(items: readonly PresetInfo[]): string[] {
  return tagFacets(items).map((f) => f.name);
}
