// SPDX-License-Identifier: AGPL-3.0-or-later
// Learn mode (docs/live.md §4.4): one line over the rig while the plug-in waits for a controller,
// watches a footswitch, then says what it bound. The control being learned is outlined. Escape
// cancels; the line goes by itself a few seconds after the binding.
import { useEffect } from 'react';
import buttons from '../../controls/Button.module.css';
import { Led } from '../../controls/Led';
import { useParamStore } from '../../params/paramStore';
import { liveActions, useLiveStore } from '../../state/liveStore';
import type { LiveBinding, LiveLearn } from '../../bridge/protocol';
import { parseTarget, sourceLabel, targetLabel } from './model';
import styles from './LearnBanner.module.css';

/** How long "bound" stays up: long enough to read a short sentence twice. */
export const DONE_SECONDS = 4;

/** The elements that stand for a target, for the outline. */
function elementsFor(target: string): HTMLElement[] {
  const t = parseTarget(target);
  if (!t) return [];
  const selector =
    t.kind === 'param'
      ? `[data-param="${CSS.escape(t.id)}"]`
      : `[data-learn="${CSS.escape(target)}"]`;
  return [...document.querySelectorAll<HTMLElement>(selector)];
}

function modeText(b: LiveBinding): string {
  if (!b.switch) return '';
  return b.mode === 'toggle' ? ': each press switches it' : ': it follows the switch';
}

function Message({ learn, binding }: { learn: LiveLearn; binding: LiveBinding | undefined }) {
  const specs = useParamStore((s) => s.specs);
  const name = targetLabel(learn.target, specs);
  if (learn.phase === 'waiting')
    return (
      <>
        Move a pedal or press a footswitch for <b>{name}</b>
      </>
    );
  if (learn.phase === 'listening')
    return (
      <>
        <b>{name}</b> on {binding ? sourceLabel(binding) : 'this controller'}. Checking whether the
        footswitch springs back…
      </>
    );
  return (
    <>
      <b>{name}</b> follows {binding ? sourceLabel(binding) : 'the controller'}
      {binding ? modeText(binding) : ''}
    </>
  );
}

export function LearnBanner() {
  const learn = useLiveStore((s) => s.state?.learn ?? null);
  const binding = useLiveStore((s) =>
    learn && learn.binding >= 0 ? s.state?.bindings[learn.binding] : undefined,
  );
  const target = learn?.target ?? null;
  const phase = learn?.phase ?? null;

  // Outline what is being learned, wherever it is drawn (a knob can be on two pages).
  useEffect(() => {
    if (!target) return;
    const marked = elementsFor(target);
    marked.forEach((el) => el.setAttribute('data-learning', ''));
    return () => marked.forEach((el) => el.removeAttribute('data-learning'));
  }, [target]);

  // Escape stops waiting; a dialog or menu that is open takes its own Escape first.
  useEffect(() => {
    if (!target) return;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || e.defaultPrevented) return;
      void liveActions.cancelLearn();
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
  }, [target]);

  useEffect(() => {
    if (phase !== 'done') return;
    const timer = setTimeout(() => void liveActions.cancelLearn(), DONE_SECONDS * 1000);
    return () => clearTimeout(timer);
  }, [phase, target]);

  return (
    <div className={styles.region} role="status" aria-live="polite">
      {learn && (
        <div className={styles.banner} data-phase={learn.phase}>
          <Led on size={6} className={learn.phase === 'done' ? undefined : styles.pulse} />
          <span className={styles.text}>
            <Message learn={learn} binding={binding} />
          </span>
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.small}`}
            onClick={() => void liveActions.cancelLearn()}
          >
            {learn.phase === 'done' ? 'Done' : 'Cancel'}
          </button>
        </div>
      )}
    </div>
  );
}
