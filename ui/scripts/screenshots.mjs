// SPDX-License-Identifier: AGPL-3.0-or-later
// Renders the built UI (vite preview) against the mock backend and saves screenshots to
// docs/images at 1200x760 @2x. Also fails on console errors, so it doubles as a smoke test.
//   pnpm build && pnpm screenshots [name ...]
import { mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { preview } from 'vite';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const outDir = resolve(root, '../docs/images');
mkdirSync(outDir, { recursive: true });

const SHOTS = {
  amp: { query: 'section=amp' },
  pedals: {
    query: 'section=pedals',
    async prepare(page) {
      await page.getByRole('switch', { name: 'Pearl on/off' }).click();
      await page.getByRole('switch', { name: 'Ripple on/off' }).click();
      await page.getByRole('switch', { name: 'Tide on/off' }).click();
    },
  },
  eq: { query: 'section=eq&preset=low-tide' },
  cab: {
    query: 'section=cab',
    async prepare(page) {
      await page.getByRole('combobox', { name: 'Mic B type' }).selectOption({ label: 'Custom IR' });
      await page.getByRole('button', { name: 'Load…' }).click();
    },
  },
  fx: { query: 'section=fx&preset=night-swim' },
  tuner: {
    query: 'overlay=tuner',
    async prepare(page) {
      await page.waitForSelector('[data-state="in-tune"]', { timeout: 15000 });
      await page.waitForTimeout(400);
    },
  },
  intonation: {
    query: 'overlay=intonation',
    async prepare(page) {
      await page.getByRole('button', { name: 'Start measuring' }).click();
      await page.getByText('Octave error').waitFor({ timeout: 20000 });
      await page.waitForTimeout(300);
    },
  },
  metronome: {
    query: 'section=amp',
    async prepare(page) {
      await page.getByRole('button', { name: 'Metronome' }).click();
      await page.getByRole('button', { name: 'Start metronome' }).click();
      await page.waitForTimeout(700);
    },
  },
  abyss: { query: 'section=amp&preset=pressure-ridge' },
  setup: { query: 'overlay=intonation' },
  settings: {
    query: 'section=pedals',
    async prepare(page) {
      await page.getByRole('button', { name: 'Settings' }).click();
      await page.waitForTimeout(300);
    },
  },
  summary: {
    query: 'overlay=intonation',
    async prepare(page) {
      await page.getByRole('button', { name: 'Start measuring' }).click();
      for (let i = 0; i < 6; i += 1) {
        await page.getByText('Octave error').waitFor({ timeout: 20000 });
        await page.getByRole('button', { name: /Next string|See summary/ }).click();
      }
      await page.getByRole('button', { name: 'Copy as text' }).waitFor();
    },
  },
  presets: {
    query: 'section=amp',
    async prepare(page) {
      await page.getByRole('button', { name: /Open preset browser/ }).click();
      await page.waitForTimeout(300);
    },
  },
};

const wanted = process.argv.slice(2);
const names = wanted.length ? wanted : ['amp', 'pedals', 'eq', 'cab', 'fx', 'tuner', 'intonation'];

const server = await preview({
  root,
  preview: { port: 4179, strictPort: false },
  logLevel: 'error',
});
const url = server.resolvedUrls?.local[0] ?? 'http://localhost:4179/';
const browser = await chromium.launch();
const problems = [];

try {
  for (const name of names) {
    const shot = SHOTS[name];
    if (!shot) throw new Error(`Unknown screenshot "${name}"`);
    const page = await browser.newPage({
      viewport: { width: 1200, height: 760 },
      deviceScaleFactor: 2,
    });
    page.on('console', (msg) => {
      if (msg.type() === 'error' || msg.type() === 'warning')
        problems.push(`${name}: ${msg.type()}: ${msg.text()}`);
    });
    page.on('pageerror', (err) => problems.push(`${name}: pageerror: ${err.message}`));
    await page.goto(`${url}?fresh&${shot.query}`);
    await page.waitForSelector('header');
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(600);
    if (shot.prepare) await shot.prepare(page);
    await page.mouse.move(1199, 759);
    const file = resolve(outDir, `${name}.png`);
    await page.screenshot({ path: file });
    console.log(`saved ${file}`);
    await page.close();
  }
} finally {
  await browser.close();
  await new Promise((r) => server.httpServer.close(r));
}

if (problems.length) {
  console.error(problems.join('\n'));
  process.exit(1);
}
