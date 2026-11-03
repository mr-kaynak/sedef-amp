// SPDX-License-Identifier: AGPL-3.0-or-later
// Section tabs double as a status strip: each shows what is switched on inside it, so the
// whole rig can be read without opening every page.
import { useRef } from 'react';
import type { KeyboardEvent, ReactNode } from 'react';
import { Led } from '../controls/Led';
import { useParamSpec, useParamValue } from '../params/useParam';
import { AMP_NAMES, effectiveCab } from '../params/schema';
import { SECTION_LABELS, SECTIONS, uiActions, useUiStore, type Section } from '../state/uiStore';
import styles from './SectionTabs.module.css';

function Leds({ ids }: { ids: string[] }) {
  return (
    <span className={styles.leds}>
      {ids.map((id) => (
        <LedFor key={id} id={id} />
      ))}
    </span>
  );
}

function LedFor({ id }: { id: string }) {
  return <Led on={useParamValue(id) >= 0.5} size={5} />;
}

function AmpStatus() {
  const amp = Math.round(useParamValue('ampModel'));
  const capture = useParamValue('ampSource') >= 0.5;
  return <span className={styles.amp}>{capture ? 'Capture' : AMP_NAMES[amp]}</span>;
}

function EqStatus() {
  const amp = AMP_NAMES[Math.round(useParamValue('ampModel'))] ?? 'Reef';
  return <Leds ids={[`eq${amp}Enabled`]} />;
}

function CabStatus() {
  const cab = effectiveCab(
    Math.round(useParamValue('ampModel')),
    useParamValue('ampCabLink'),
    Math.round(useParamValue('cabModel')),
  );
  const label = useParamSpec('cabModel').choices[cab] ?? '';
  return <span className={styles.meta}>{label.split(' ')[0]}</span>;
}

const STATUS: Record<Section, () => ReactNode> = {
  pedals: () => (
    <Leds ids={['gateEnabled', 'tideEnabled', 'pearlEnabled', 'coralEnabled', 'rippleEnabled']} />
  ),
  amp: () => <AmpStatus />,
  eq: () => <EqStatus />,
  cab: () => <CabStatus />,
  fx: () => <Leds ids={['doublerEnabled', 'driftEnabled', 'fathomEnabled']} />,
};

function choose(id: Section) {
  uiActions.setSection(id);
  uiActions.openOverlay(null);
}

export function SectionTabs() {
  const section = useUiStore((s) => s.section);
  const overlay = useUiStore((s) => s.overlay);
  const refs = useRef<(HTMLButtonElement | null)[]>([]);

  const onKeyDown = (e: KeyboardEvent) => {
    const index = SECTIONS.indexOf(section);
    const next =
      e.key === 'ArrowRight'
        ? index + 1
        : e.key === 'ArrowLeft'
          ? index - 1
          : e.key === 'Home'
            ? 0
            : e.key === 'End'
              ? SECTIONS.length - 1
              : null;
    if (next === null) return;
    e.preventDefault();
    const wrapped = (next + SECTIONS.length) % SECTIONS.length;
    const id = SECTIONS[wrapped];
    if (!id) return;
    choose(id);
    refs.current[wrapped]?.focus();
  };

  return (
    <nav className={styles.tabs} role="tablist" aria-label="Rig sections" onKeyDown={onKeyDown}>
      {SECTIONS.map((id, i) => {
        const Status = STATUS[id];
        const selected = section === id && !overlay;
        return (
          <button
            key={id}
            ref={(el) => {
              refs.current[i] = el;
            }}
            type="button"
            role="tab"
            id={`tab-${id}`}
            aria-selected={selected}
            aria-controls="section-panel"
            aria-keyshortcuts={String(i + 1)}
            tabIndex={section === id ? 0 : -1}
            className={styles.tab}
            onClick={() => choose(id)}
          >
            <span className={styles.key} aria-hidden="true">
              {i + 1}
            </span>
            <span className={styles.label}>{SECTION_LABELS[id]}</span>
            <Status />
          </button>
        );
      })}
    </nav>
  );
}
