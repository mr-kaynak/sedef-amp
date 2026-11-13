// SPDX-License-Identifier: AGPL-3.0-or-later
// The scene bar and the MIDI button, at the right of the section tabs (docs/live.md §6.1): on
// every page, at every window size. Scenes are a radio group (one tab stop, arrows select);
// right-click or the context-menu key on a scene learns a footswitch for it or copies the scene
// in use into it (ControlMenu).
//
// The row also holds Transpose. While it shifts, its latency note and Back to 0 take the width the
// eight buttons need (1200 px: tabs to 610, Transpose 356 px, the bar 357), so the bar folds into
// one "Scene 3" button that opens the eight.
import { useCallback, useEffect, useRef, useState } from 'react';
import { Icon } from '../../controls/Icon';
import { Popover } from '../../controls/Popover';
import { Led } from '../../controls/Led';
import { useParamValue } from '../../params/useParam';
import buttons from '../../controls/Button.module.css';
import nacre from '../../controls/NacreFill.module.css';
import { useRadioGroup } from '../../controls/useRadioGroup';
import { bindingFor, liveActions, useLiveStore } from '../../state/liveStore';
import { MidiPanel } from './MidiPanel';
import { sceneTarget, sourceLabel } from './model';
import styles from './LiveBar.module.css';

function SceneBar({ labelled = true }: { labelled?: boolean }) {
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
      {labelled && (
        <span className={styles.label} id="scene-bar-label" aria-hidden="true">
          Scenes
        </span>
      )}
      <div
        role="radiogroup"
        {...(labelled ? { 'aria-labelledby': 'scene-bar-label' } : { 'aria-label': 'Scenes' })}
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

/** The bar folded: the scene in use, and the eight a click away. */
function SceneChip() {
  const active = useLiveStore((s) => s.state?.scenes.active ?? 0);
  const [open, setOpen] = useState(false);
  const anchor = useRef<HTMLButtonElement>(null);
  const close = useCallback(() => setOpen(false), []);
  return (
    <div className={styles.anchor}>
      <button
        ref={anchor}
        type="button"
        className={`${buttons.ghost} ${buttons.small} ${styles.chip}`}
        aria-haspopup="dialog"
        aria-expanded={open}
        aria-label={`Scene ${active + 1}, choose a scene`}
        onClick={() => setOpen((o) => !o)}
      >
        Scene {active + 1}
        <Icon name={open ? 'up' : 'down'} size={12} />
      </button>
      <Popover
        open={open}
        onClose={close}
        anchorRef={anchor}
        label="Scenes"
        className={styles.scenePopover}
      >
        <SceneBar labelled={false} />
      </Popover>
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
  const [open, setOpen] = useState(false);
  const anchor = useRef<HTMLButtonElement>(null);
  const close = useCallback(() => setOpen(false), []);
  const folded = useParamValue('transpose') !== 0;
  if (!supported) return <div className={styles.bar} />;

  return (
    <div className={styles.bar}>
      {folded ? <SceneChip /> : <SceneBar />}
      <div className={styles.anchor}>
        <button
          ref={anchor}
          type="button"
          className={`${buttons.ghost} ${buttons.small} ${styles.midi}`}
          aria-haspopup="dialog"
          aria-expanded={open}
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
