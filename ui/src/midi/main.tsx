// SPDX-License-Identifier: AGPL-3.0-or-later
// Sedef MIDI's UI entry (midi.html). In a plain browser it runs on the mock backend; add
// ?host=standalone, ?host=instrument or ?host=plugin to preview each variant.
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import '../styles/fonts';
import '../styles/tokens.css';
import '../styles/global.css';
import { MidiApp } from './App';
import { createMidiBackend } from './backend';
import { connectMidi } from './store';

async function main() {
  const host = new URLSearchParams(window.location.search).get('host');
  const backend = await createMidiBackend({
    isStandalone: host === 'standalone',
    isInstrument: host === 'instrument',
    wrapperType: host === 'plugin' ? 'VST3' : host === 'instrument' ? 'AudioUnit' : undefined,
    autoPlay: !new URLSearchParams(window.location.search).has('still'),
  });
  await connectMidi(backend);
  const root = document.getElementById('root');
  if (!root) throw new Error('Missing #root element');
  createRoot(root).render(
    <StrictMode>
      <MidiApp />
    </StrictMode>,
  );
}

void main();
