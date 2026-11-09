// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared plumbing for the screenshot and capture scripts: serve dist/ with vite preview, open
// the 1200x760 canvas in headless Chromium, and collect console problems so a broken UI fails
// the run instead of producing pictures of an error.
import { chromium } from 'playwright';
import { preview } from 'vite';

export const CANVAS = { width: 1200, height: 760 };

/**
 * `configFile` picks the build to serve (Sedef MIDI's is vite.midi.config.ts, with its own page),
 * `page` the HTML file to open and `canvas` the viewport.
 */
export async function startSession(
  root,
  { scale = 2, configFile = undefined, page: html = '', canvas = CANVAS } = {},
) {
  const server = await preview({
    root,
    ...(configFile ? { configFile } : {}),
    preview: { port: 4179, strictPort: false },
    logLevel: 'error',
  });
  const base = server.resolvedUrls?.local[0] ?? 'http://localhost:4179/';
  const browser = await chromium.launch();
  const problems = [];

  /**
   * Opens a page at ?<query>, waits for fonts and the first frames, runs prepare(page). The
   * pointer is then parked in a corner, unless the shot shows what it points at (keepPointer).
   */
  async function open(name, query, prepare, { keepPointer = false } = {}) {
    const page = await browser.newPage({ viewport: canvas, deviceScaleFactor: scale });
    page.on('console', (msg) => {
      if (msg.type() === 'error' || msg.type() === 'warning')
        problems.push(`${name}: ${msg.type()}: ${msg.text()}`);
    });
    page.on('pageerror', (err) => problems.push(`${name}: pageerror: ${err.message}`));
    await page.goto(`${base}${html}?${query}`);
    await page.waitForSelector('header');
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(500);
    if (prepare) await prepare(page);
    if (!keepPointer) await page.mouse.move(canvas.width - 1, canvas.height - 1);
    return page;
  }

  async function close() {
    await browser.close();
    await new Promise((resolve) => server.httpServer.close(resolve));
  }

  return { open, close, problems };
}
