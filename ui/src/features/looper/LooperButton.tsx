// SPDX-License-Identifier: AGPL-3.0-or-later
// The header's Looper button: opens the panel and, with the panel closed, still shows the
// looper's state (a small ring in its colour) and announces its changes, so a player using the
// L key can follow it without the panel.
import { useEffect, useState } from 'react';
import type { Ref } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon, type IconName } from '../../controls/Icon';
import type { LooperTelemetry } from '../../bridge/protocol';
import { useAppStore } from '../../state/appStore';
import { dropClearNotice } from './actions';
import { LoopRing } from './LoopRing';
import { announcement, stateTitle, toneOf } from './model';
import { useLooper } from './useLooper';
import styles from './LooperButton.module.css';

/** The state as a shape as well as a colour: a dot records, a plus overdubs, a triangle plays,
 *  a square is stopped; hollow while it waits for the 1. */
function badgeOf(t: LooperTelemetry): { icon: IconName; filled: boolean } | null {
  switch (t.state) {
    case 'recording':
    case 'closing':
      return { icon: 'record', filled: true };
    case 'overdubbing':
      return { icon: 'plus', filled: false };
    case 'playing':
      return { icon: 'play', filled: true };
    case 'stopped':
      return { icon: 'stop', filled: true };
    case 'armed':
      return { icon: t.armedForPlay ? 'play' : 'record', filled: false };
    default:
      return null;
  }
}

export function LooperButton({
  expanded,
  onToggle,
  ref,
}: {
  expanded: boolean;
  onToggle: () => void;
  ref?: Ref<HTMLButtonElement>;
}) {
  const rate = useAppStore((s) => s.session.sampleRate);
  const t = useLooper(rate);
  const [previous, setPrevious] = useState<LooperTelemetry | null>(null);
  const [said, setSaid] = useState('');
  const active = t.state !== 'unavailable' && t.state !== 'empty';
  const badge = badgeOf(t);
  if (previous !== t) {
    // A new state is announced once (the region is polite, so it never interrupts).
    const text = announcement(previous, t);
    setPrevious(t);
    if (text) setSaid(text);
  }

  // A new take ends what the clear notice's Undo could bring back.
  useEffect(() => {
    if (t.state === 'recording') dropClearNotice();
  }, [t.state]);

  return (
    <>
      <button
        ref={ref}
        type="button"
        className={`${buttons.ghost} ${buttons.icon} ${styles.button}`}
        data-tone={toneOf(t.state)}
        aria-label={active ? `Looper, ${stateTitle(t).toLowerCase()}` : 'Looper'}
        title="Looper (L)"
        aria-haspopup="dialog"
        aria-expanded={expanded}
        onClick={onToggle}
      >
        {active && <LoopRing size={28} stroke={2} rate={rate} className={styles.ring} />}
        <Icon name="loop" />
        {badge && (
          <span className={styles.badge} data-state={t.state}>
            <Icon name={badge.icon} size={8} filled={badge.filled} stroke={1.5} />
          </span>
        )}
      </button>
      <span className="visually-hidden" role="status" aria-live="polite">
        {said}
      </span>
    </>
  );
}
