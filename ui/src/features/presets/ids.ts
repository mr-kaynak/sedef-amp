// SPDX-License-Identifier: AGPL-3.0-or-later
// DOM ids the browser's search field and list share (aria-controls, aria-activedescendant).

export const LIST_ID = 'preset-browser-list';

/**
 * DOM id of a row. Preset ids hold characters that are not valid in selectors (":", "/",
 * spaces), so everything else is written as _<hex>_, which keeps distinct ids distinct.
 */
export function optionId(presetId: string): string {
  return `preset-option-${presetId.replace(/[^a-zA-Z0-9-]/g, (c) => `_${c.charCodeAt(0).toString(16)}_`)}`;
}
