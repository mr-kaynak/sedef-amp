// SPDX-License-Identifier: AGPL-3.0-or-later
// The preset browser's model, as pure functions: which presets a library view holds, the
// categories and tags it offers, search, sorting, and the signal-chain summary of a row.
import type { PresetBlock, PresetInfo } from '../../bridge/protocol';
import { AMP_NAMES, bundledSchema } from '../../params/schema';

export type Library = 'all' | 'favorites' | 'recent' | 'factory' | 'user';
export type SortKey = 'name' | 'recent' | 'category';

export interface BrowseFilter {
  library: Library;
  /** A folder name, or null for every category. */
  category: string | null;
  /** Selected tag chips; a preset must carry all of them. */
  tags: readonly string[];
  query: string;
  sort: SortKey;
}

export const DEFAULT_FILTER: BrowseFilter = {
  library: 'all',
  category: null,
  tags: [],
  query: '',
  sort: 'name',
};

export const LIBRARIES: readonly { id: Library; label: string }[] = [
  { id: 'all', label: 'All presets' },
  { id: 'favorites', label: 'Favourites' },
  { id: 'recent', label: 'Recent' },
  { id: 'factory', label: 'Factory' },
  { id: 'user', label: 'User' },
];

export const SORTS: readonly { id: SortKey; label: string }[] = [
  { id: 'name', label: 'Name' },
  { id: 'recent', label: 'Recent' },
  { id: 'category', label: 'Category' },
];

export interface Facet {
  name: string;
  count: number;
}

const natural = (a: string, b: string) =>
  a.localeCompare(b, 'en', { numeric: true, sensitivity: 'base' });

export function inLibrary(p: PresetInfo, library: Library, recent: readonly string[]): boolean {
  switch (library) {
    case 'all':
      return true;
    case 'favorites':
      return p.favorite;
    case 'recent':
      return recent.includes(p.id);
    case 'factory':
      return p.category === 'Factory';
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
    factory: count('factory'),
    user: count('user'),
  };
}

/** The categories (folders) of the presets in a library, alphabetically. Root presets have none. */
export function categoryFacets(
  items: readonly PresetInfo[],
  library: Library,
  recent: readonly string[],
): Facet[] {
  const counts = new Map<string, number>();
  for (const p of items)
    if (p.folder && inLibrary(p, library, recent))
      counts.set(p.folder, (counts.get(p.folder) ?? 0) + 1);
  return [...counts.entries()]
    .map(([name, count]) => ({ name, count }))
    .sort((a, b) => natural(a.name, b.name));
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

export const BLOCK_LABELS: Record<PresetBlock, string> = {
  gate: 'Gate',
  tide: 'Tide',
  pearl: 'Pearl',
  coral: 'Coral',
  ripple: 'Ripple',
  eq: 'EQ',
  doubler: 'Doubler',
  drift: 'Drift',
  fathom: 'Fathom',
};

/** Cab names in cabModel order ("1x12 Open Back"…), and their short form ("1x12"). */
export const CAB_NAMES: readonly string[] = bundledSchema.find((s) => s.id === 'cabModel')
  ?.choices ?? ['1x12', '2x12', '4x12'];
export const CAB_LABELS: readonly string[] = CAB_NAMES.map((c) => c.split(' ')[0] ?? c);

const PRE_AMP: readonly PresetBlock[] = ['tide', 'pearl', 'coral', 'ripple'];

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
 * The switched-on chain in signal order, pedals then amp and cab then post effects. The gate is
 * left out: it is on in almost every preset and says nothing about the sound.
 */
export function chainParts(p: Pick<PresetInfo, 'amp' | 'cab' | 'blocks'>): ChainPart[] {
  const block = (b: PresetBlock): ChainPart => ({ kind: 'block', label: BLOCK_LABELS[b] });
  const pre = p.blocks.filter((b) => PRE_AMP.includes(b));
  const post = p.blocks.filter((b) => b !== 'gate' && !PRE_AMP.includes(b));
  return [
    ...pre.map(block),
    { kind: 'amp', label: ampName(p), detail: CAB_LABELS[p.cab] ?? '' },
    ...post.map(block),
  ];
}

/** The chain as the detail pane lists it: pedals, amp, cab, then EQ and effects. */
export function chainGroups(p: Pick<PresetInfo, 'amp' | 'cab' | 'blocks'>): {
  pedals: string[];
  amp: string;
  cab: string;
  fx: string[];
} {
  const parts = chainParts(p);
  const at = parts.findIndex((part) => part.kind === 'amp');
  return {
    pedals: parts.slice(0, at).map((part) => part.label),
    amp: ampName(p),
    cab: CAB_NAMES[p.cab] ?? '',
    fx: parts.slice(at + 1).map((part) => part.label),
  };
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
    ...p.blocks.map((b) => BLOCK_LABELS[b]),
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

function compare(sort: SortKey, recent: readonly string[]) {
  const byName = (a: PresetInfo, b: PresetInfo) =>
    natural(a.name, b.name) || (a.category === b.category ? 0 : a.category === 'Factory' ? -1 : 1);
  if (sort === 'name') return byName;
  if (sort === 'category') {
    // Presets without a folder sort last.
    return (a: PresetInfo, b: PresetInfo) =>
      (a.folder ? 0 : 1) - (b.folder ? 0 : 1) || natural(a.folder, b.folder) || byName(a, b);
  }
  const rank = (p: PresetInfo) => {
    const i = recent.indexOf(p.id);
    return i < 0 ? Number.MAX_SAFE_INTEGER : i;
  };
  return (a: PresetInfo, b: PresetInfo) => rank(a) - rank(b) || byName(a, b);
}

/** The presets a filter shows, in display order. */
export function browse(
  items: readonly PresetInfo[],
  filter: BrowseFilter,
  recent: readonly string[],
): PresetInfo[] {
  return items
    .filter(
      (p) =>
        inLibrary(p, filter.library, recent) &&
        (filter.category === null || p.folder === filter.category) &&
        filter.tags.every((t) => p.tags.includes(t)) &&
        matchesQuery(p, filter.query),
    )
    .sort(compare(filter.sort, recent));
}

/** Every folder in use, for the save form's category picker. */
export function allCategories(items: readonly PresetInfo[]): string[] {
  return [...new Set(items.map((p) => p.folder).filter(Boolean))].sort(natural);
}

/** Every tag in use, most common first, for suggestions. */
export function allTags(items: readonly PresetInfo[]): string[] {
  return tagFacets(items).map((f) => f.name);
}
