// SPDX-License-Identifier: AGPL-3.0-or-later
// User preset files, named the way plugin/src/PresetManager.cpp names them: the preset name made
// legal as a file name (juce::File::createLegalFileName) plus the extension, in the folder of the
// preset. Ids are "user:<relative path>".
import type { PresetInfo } from '../bridge/protocol';

const ILLEGAL = /["#@,;:<>*^|?\\/]/g;

/** The id a user preset gets when the preset `id` is renamed to `name`. */
export function renamedPresetId(id: string, name: string): string {
  const folder = id.replace(/^user:/, '').replace(/[^/]*$/, '');
  return `user:${folder}${name.trim().replace(ILLEGAL, '')}.sedefpreset`;
}

/**
 * True when renaming would land on another preset's file, which the plugin refuses. Compared
 * without case, as on the default macOS and Windows file systems.
 */
export function renameClashes(items: readonly PresetInfo[], id: string, name: string): boolean {
  const target = renamedPresetId(id, name).toLowerCase();
  return items.some((p) => p.id !== id && p.id.toLowerCase() === target);
}
