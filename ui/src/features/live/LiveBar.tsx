// SPDX-License-Identifier: AGPL-3.0-or-later
// The scene bar and the MIDI button, at the right of the section tabs (docs/live.md §6.1): on
// every page, at every window size. Scenes are a radio group (one tab stop, arrows select);
// right-click or the context-menu key on a scene learns a footswitch for it or copies the scene
// in use into it (ControlMenu).
import { useCallback, useEffect, useRef, useState } from 'react';
import { Popover } from '../../controls/Popover';
import { Led } from '../../controls/Led';
import buttons from '../../controls/Button.module.css';
import nacre from '../../controls/NacreFill.module.css';
import { useRadioGroup } from '../../controls/useRadioGroup';
import { bindingFor, liveActions, useLiveStore } from '../../state/liveStore';
import { MidiPanel } from './MidiPanel';
import { sceneTarget, sourceLabel } from './model';
import styles from './LiveBar.module.css';

function SceneBar() {
  const scenes = useLiveStore((s) => s.state?.scenes);
  const live = useLiveStore((s) => s.state);
  const count = scenes?.count ?? 0;
  const active = scenes?.active ?? 0;
  const { onKeyDown, radio } = useRadioGroup(count, active, (index) => {
    void liveActions.selectScene(index);
  });
  if (!scenes) return null;

  return (
    <div className={styles.scenes}>
      <span className={styles.label} id="scene-bar-label" aria-hidden="true">
        Scenes
      </span>
      <div
        role="radiogroup"
        aria-labelledby="scene-bar-label"
        className={styles.track}
        onKeyDown={onKeyDown}
      >
        {Array.from({ length: count }, (_, i) => {
          const made = scenes.made[i] ?? false;
          const binding = bindingFor(live, sceneTarget(i));
          const name = `Scene ${i + 1}`;
          return (
            <button
              key={i}
              type="button"
              className={`${nacre.fill} ${styles.scene}`}
              data-learn={sceneTarget(i)}
              data-made={made || undefined}
              aria-label={made ? name : `${name}, empty`}
              title={[
                made ? name : `${name}: empty, starts as a copy`,
                binding && sourceLabel(binding),
              ]
                .filter(Boolean)
                .join(' · ')}
              {...radio(i)}
            >
              {i + 1}
            </button>
          );
        })}
      </div>
    </div>
  );
}

/** Lights for a moment whenever a control or program change arrives. */
function ActivityLed() {
  const count = useLiveStore((s) => s.activity?.count ?? 0);
  const [lit, setLit] = useState(false);
  const first = useRef(count);
  useEffect(() => {
    if (count === first.current) return;
    setLit(true);
    const timer = setTimeout(() => setLit(false), 140);
    return () => clearTimeout(timer);
  }, [count]);
  return <Led on={lit} size={6} />;
}

export function LiveBar() {
  const supported = useLiveStore((s) => s.state !== null);
  const learning = useLiveStore((s) => s.state?.learn !== null && s.state?.learn !== undefined);
  const [open, setOpen] = useState(false);
  const anchor = useRef<HTMLButtonElement>(null);
  const close = useCallback(() => setOpen(false), []);
  if (!supported) return <div className={styles.bar} />;

  return (
    <div className={styles.bar}>
      <SceneBar />
      <div className={styles.anchor}>
        <button
          ref={anchor}
          type="button"
          className={`${buttons.ghost} ${buttons.small} ${styles.midi}`}
          aria-haspopup="dialog"
          aria-expanded={open}
          data-learning-active={learning || undefined}
          onClick={() => setOpen((o) => !o)}
        >
          <ActivityLed />
          MIDI
        </button>
        <Popover
          open={open}
          onClose={close}
          anchorRef={anchor}
          label="MIDI"
          className={styles.popover}
        >
          <MidiPanel onClose={close} />
        </Popover>
      </div>
    </div>
  );
}
