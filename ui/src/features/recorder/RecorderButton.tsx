// SPDX-License-Identifier: AGPL-3.0-or-later
// The header's Recorder button: opens the panel and, with the panel closed, still shows a take
// recording (the dot filled red and breathing) or a file re-amping (a corner badge), and
// announces a take starting and ending, so a player using the R key can follow it without the
// panel. The elapsed time is in the status bar, which the header has no room for.
import { useState } from 'react';
import type { Ref } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { telemetry, useLiveSelector } from '../../state/live';
import { spoken } from './format';
import { DROPOUT_ANNOUNCEMENT, takeAnnouncement, type TakeStatus } from './model';
import { useRecorderStore } from './store';
import styles from './RecorderButton.module.css';

export function RecorderButton({
  expanded,
  onToggle,
  ref,
}: {
  expanded: boolean;
  onToggle: () => void;
  ref?: Ref<HTMLButtonElement>;
}) {
  const state = useRecorderStore((s) => s.state);
  const status: TakeStatus = state?.recorder.status ?? 'idle';
  const take = state?.recorder.take ?? null;
  const recording = status !== 'idle';
  const seconds = useLiveSelector(telemetry, (t) => Math.floor(t?.recorder?.seconds ?? 0));
  const reamping = useLiveSelector(telemetry, (t) => t?.reamp?.playing ?? false);
  const dropped = useLiveSelector(
    telemetry,
    (t) => t?.recorder?.status === 'recording' && t.recorder.dropouts > 0,
  );
  const [previous, setPrevious] = useState<TakeStatus | null>(null);
  const [wasDropped, setWasDropped] = useState(false);
  const [said, setSaid] = useState('');
  if (previous !== status) {
    const text = takeAnnouncement(previous, status, take);
    setPrevious(status);
    if (text) setSaid(text);
  }
  if (wasDropped !== dropped) {
    setWasDropped(dropped);
    if (dropped) setSaid(DROPOUT_ANNOUNCEMENT);
  }

  const label = recording
    ? `Recorder, recording, ${spoken(seconds)}`
    : reamping
      ? 'Recorder, re-amping'
      : 'Recorder';

  return (
    <>
      <button
        ref={ref}
        type="button"
        className={`${buttons.ghost} ${buttons.icon} ${styles.button}`}
        data-recording={recording || undefined}
        aria-label={label}
        title="Recorder (R records)"
        aria-haspopup="dialog"
        aria-expanded={expanded}
        onClick={onToggle}
      >
        <Icon name="record" filled={recording} stroke={ICON_STROKE_LABEL} className={styles.dot} />
        {reamping && !recording && (
          <span className={styles.badge}>
            <Icon name="play" size={8} filled stroke={1.5} />
          </span>
        )}
      </button>
      <span className="visually-hidden" role="status" aria-live="polite">
        {said}
      </span>
    </>
  );
}
