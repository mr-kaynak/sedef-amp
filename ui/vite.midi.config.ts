// SPDX-License-Identifier: AGPL-3.0-or-later
// Sedef MIDI's UI: the same rules as the Sedef Amp UI (vite.config.ts), its own page and output.
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    outDir: 'dist-midi',
    assetsDir: 'assets',
    target: ['safari15', 'chrome100', 'edge100'],
    assetsInlineLimit: 0,
    sourcemap: false,
    rollupOptions: { input: 'midi.html' },
  },
  server: { port: 5174, strictPort: false, open: '/midi.html' },
});
