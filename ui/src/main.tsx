// SPDX-License-Identifier: AGPL-3.0-or-later
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import './styles/fonts';
import './styles/tokens.css';
import './styles/global.css';
import { App } from './app/App';
import { connect, createBackend } from './app/connect';
import { readDemoFlags } from './app/demo';
import { installDemoHook } from './app/demoHook';
import { demoTab } from './bridge/mock/recorderSim';
import { MockBackend } from './bridge/mockBackend';
import { recorderUi } from './features/recorder/store';
import { uiActions } from './state/uiStore';

async function main() {
  const flags = readDemoFlags();
  const backend = await createBackend(flags);
  await connect(backend);
  if (flags.section) uiActions.setSection(flags.section);
  if (flags.overlay) uiActions.openOverlay(flags.overlay);
  if (flags.recorder) recorderUi.setTab(demoTab(flags.recorder));
  if (backend instanceof MockBackend) {
    if (flags.freeze) backend.publishStill();
    if (flags.demo) installDemoHook();
  }
  if (!flags.grain) document.documentElement.dataset.grain = 'off';

  const root = document.getElementById('root');
  if (!root) throw new Error('Missing #root element');
  createRoot(root).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
}

void main();
