// SPDX-License-Identifier: AGPL-3.0-or-later
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

const here = fileURLToPath(new URL('.', import.meta.url));
// The mock backend bundles the real factory presets from the repository.
const presetsDir = fileURLToPath(new URL('../presets', import.meta.url));

// The plugin serves dist/ from a zip embedded in the binary by relative path, so every asset
// URL must be relative (base './') and nothing may be fetched from the network.
export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
    target: ['safari15', 'chrome100', 'edge100'],
    // Fonts must stay as files: WebKit in some hosts refuses large data: URLs for fonts.
    assetsInlineLimit: 0,
    sourcemap: false,
  },
  server: { port: 5173, strictPort: false, fs: { allow: [here, presetsDir] } },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./src/test/setup.ts'],
    // jsdom applies no stylesheets, so tests that pin a CSS rule read the file with `?raw`; only
    // those imports go through Vite's pipeline (the modules themselves keep unscoped names).
    css: { include: [/\.css\?raw$/], modules: { classNameStrategy: 'non-scoped' } },
    include: ['src/**/*.test.{ts,tsx}'],
  },
});
