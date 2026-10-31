// SPDX-License-Identifier: AGPL-3.0-or-later
// The schema bundled for the browser mock and tests. Inside the plugin the schema arrives with
// sedef_hello and is the authority; both come from engine/include/sedef/params/ParameterList.inc.
import type { ParamSpec } from '../bridge/protocol';
import generated from '../generated/parameters.json';

interface GeneratedSchema {
  version: number;
  parameters: ParamSpec[];
}

export const bundledSchema: readonly ParamSpec[] = (generated as GeneratedSchema).parameters;

export function defaultValues(schema: readonly ParamSpec[]): Record<string, number> {
  return Object.fromEntries(schema.map((p) => [p.id, p.default]));
}

export function indexSchema(schema: readonly ParamSpec[]): Record<string, ParamSpec> {
  return Object.fromEntries(schema.map((p) => [p.id, p]));
}

export const AMP_NAMES = ['Lagoon', 'Reef', 'Abyss'] as const;
export type AmpName = (typeof AMP_NAMES)[number];

/** The cab in effect: follows the amp while ampCabLink is on (see architecture §6). */
export function effectiveCab(ampModel: number, ampCabLink: number, cabModel: number): number {
  return ampCabLink >= 0.5 ? ampModel : cabModel;
}
