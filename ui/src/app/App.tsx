// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ComponentType } from 'react';
import { Header } from '../features/header/Header';
import { AmpSection } from '../features/amp/AmpSection';
import { CabSection } from '../features/cab/CabSection';
import { EqSection } from '../features/eq/EqSection';
import { FxSection } from '../features/fx/FxSection';
import { IntonationWorkbench } from '../features/intonation/IntonationWorkbench';
import { PedalsSection } from '../features/pedals/PedalsSection';
import { PresetBrowser } from '../features/presets/PresetBrowser';
import { useBrowserStore } from '../features/presets/session';
import { TunerOverlay } from '../features/tuner/TunerOverlay';
import { useAppStore } from '../state/appStore';
import { useUiStore, type Section } from '../state/uiStore';
import { SectionTabs } from './SectionTabs';
import { ErrorBoundary } from './ErrorBoundary';
import { Stage } from './Stage';
import { StatusBar } from './StatusBar';
import { Toasts } from './Toasts';
import { useShortcuts } from './useShortcuts';
import styles from './App.module.css';

const SECTION_VIEWS: Record<Section, ComponentType> = {
  pedals: PedalsSection,
  amp: AmpSection,
  eq: EqSection,
  cab: CabSection,
  fx: FxSection,
};

export function App() {
  useShortcuts();
  const section = useUiStore((s) => s.section);
  const overlay = useUiStore((s) => s.overlay);
  const message = useAppStore((s) => s.message);
  const browsing = useBrowserStore((s) => s.open);
  const View = SECTION_VIEWS[section];

  return (
    <Stage>
      <div className={styles.app}>
        <Header />
        <div className={styles.body}>
          {/* The preset browser covers the rig; what it covers is out of reach until it closes. */}
          <div className={styles.rig} inert={browsing}>
            <SectionTabs />
            <main className={styles.main}>
              {message && (
                <p className={styles.message} role="alert">
                  {message}
                </p>
              )}
              <div
                id="section-panel"
                role="tabpanel"
                aria-labelledby={`tab-${section}`}
                className={styles.panel}
                hidden={overlay !== null}
              >
                <ErrorBoundary resetKey={section}>
                  <View />
                </ErrorBoundary>
              </div>
              <ErrorBoundary resetKey={overlay ?? 'none'}>
                {overlay === 'tuner' && <TunerOverlay />}
                {overlay === 'intonation' && <IntonationWorkbench />}
              </ErrorBoundary>
            </main>
          </div>
          <ErrorBoundary resetKey="presets">
            <PresetBrowser />
          </ErrorBoundary>
          <Toasts />
        </div>
        <StatusBar />
      </div>
    </Stage>
  );
}
