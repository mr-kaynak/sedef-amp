// SPDX-License-Identifier: AGPL-3.0-or-later
// User preset files, named the way plugin/src/PresetManager.cpp names them (safeFileName): the
// preset name made legal as a file name, plus the extension, in the folder of the preset. Ids are
// "user:<relative path>".
import type { PresetDetails, PresetInfo } from '../bridge/protocol';

const ILLEGAL = /["#@,;:<>*^|?\\/]/g;
const EXTENSION = '.sedefpreset';

/**
 * PresetManager::safeFileName for the characters a preset name can realistically hold: no
 * characters the file system refuses, no leading dots (".." or a hidden file), no trailing dots
 * or spaces.
 */
export function legalFileName(name: string): string {
  return name
    .trim()
    .replace(ILLEGAL, '')
    .replace(/^[. ]+|[. ]+$/g, '');
}

/** The id a user preset gets when the preset `id` is renamed to `name`. */
export function renamedPresetId(id: string, name: string): string {
  const folder = id.replace(/^user:/, '').replace(/[^/]*$/, '');
  return `user:${folder}${legalFileName(name) || 'Untitled'}${EXTENSION}`;
}

/** The id "Save as" gives a preset called `name` in `folder` (empty for the root). */
export function savedPresetId(name: string, folder: string): string {
  const dir = legalFileName(folder);
  return `user:${dir ? `${dir}/` : ''}${legalFileName(name) || 'Untitled'}${EXTENSION}`;
}

/**
 * True when renaming would land on another preset's file, which the plugin refuses. Compared
 * without case, as on the default macOS and Windows file systems.
 */
export function renameClashes(items: readonly PresetInfo[], id: string, name: string): boolean {
  const target = renamedPresetId(id, name).toLowerCase();
  return items.some((p) => p.id !== id && p.id.toLowerCase() === target);
}

/** The user preset that saving `name` into `folder` would replace, if any (compared without case). */
export function saveTarget(
  items: readonly PresetInfo[],
  name: string,
  folder: string,
): PresetInfo | undefined {
  const target = savedPresetId(name, folder).toLowerCase();
  return items.find((p) => p.category === 'User' && p.id.toLowerCase() === target);
}

export const MAX_TAGS = 8;
export const MAX_TAG_LENGTH = 24;
export const MAX_DESCRIPTION = 140;

/** Tags as the plugin stores them: trimmed, lower case, unique, at most 8 of up to 24 characters. */
export function cleanTags(tags: readonly string[]): string[] {
  const out: string[] = [];
  for (const raw of tags) {
    const tag = raw.trim().toLowerCase().slice(0, MAX_TAG_LENGTH).trim();
    if (tag && !out.includes(tag) && out.length < MAX_TAGS) out.push(tag);
  }
  return out;
}

/** One line of at most 140 characters, as the plugin stores it. */
export function cleanDescription(text: string): string {
  return text
    .replace(/[\r\n\t]/g, ' ')
    .trim()
    .slice(0, MAX_DESCRIPTION)
    .trim();
}

export function cleanDetails(details: PresetDetails): PresetDetails {
  return { tags: cleanTags(details.tags), description: cleanDescription(details.description) };
}
