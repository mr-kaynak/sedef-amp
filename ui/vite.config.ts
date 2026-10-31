// SPDX-License-Identifier: AGPL-3.0-or-later
import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

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
  server: { port: 5173, strictPort: false },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./src/test/setup.ts'],
    css: { modules: { classNameStrategy: 'non-scoped' } },
    include: ['src/**/*.test.{ts,tsx}'],
  },
});
