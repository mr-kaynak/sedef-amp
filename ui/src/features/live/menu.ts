// SPDX-License-Identifier: AGPL-3.0-or-later
// What a control's menu offers (ControlMenu.tsx, docs/live.md §3).
import type { LiveState, ParamSpec } from '../../bridge/protocol';
import { bindingFor, liveActions } from '../../state/liveStore';
import { canRememberInScenes, paramTarget, parseTarget, sourceLabel } from './model';

export interface MenuItem {
  label: string;
  run: () => void;
  disabled?: boolean;
}

/** The target an element stands for: "param:<id>" or its data-learn value. */
export function targetOf(el: HTMLElement): string | null {
  const learn = el.dataset.learn;
  if (learn) return learn;
  const id = el.dataset.param;
  return id ? paramTarget(id) : null;
}

/** What the menu offers for a target, in order. */
export function menuItems(
  target: string,
  state: LiveState,
  specs: Record<string, ParamSpec>,
): MenuItem[] {
  const t = parseTarget(target);
  if (!t) return [];
  const bound = bindingFor(state, target);
  const items: MenuItem[] = [
    {
      label: bound ? 'Learn MIDI again' : 'Learn MIDI',
      run: () => void liveActions.learn(target),
    },
  ];
  if (bound)
    items.push({
      label: `Forget ${sourceLabel(bound)}`,
      run: () => void liveActions.forget(target),
    });
  if (t.kind === 'param') {
    const spec = specs[t.id];
    if (spec && canRememberInScenes(spec)) {
      const kept = state.scenes.values.includes(t.id);
      const full = state.scenes.values.length >= state.scenes.maxValues;
      items.push(
        kept
          ? { label: 'Same in every scene', run: () => void liveActions.remember(t.id, false) }
          : {
              label: full
                ? `Remember in scenes (${state.scenes.maxValues} kept already)`
                : 'Remember in scenes',
              run: () => void liveActions.remember(t.id, true),
              disabled: full,
            },
      );
    }
  }
  if (t.kind === 'scene' && t.scene !== state.scenes.active)
    items.push({
      label: `Copy scene ${state.scenes.active + 1} here`,
      run: () => void liveActions.copyScene(t.scene),
    });
  return items;
}
