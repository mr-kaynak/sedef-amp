// SPDX-License-Identifier: AGPL-3.0-or-later
// Renders the built UI (vite preview) against the mock backend and saves screenshots to
// docs/images at 1200x760 @2x. Also fails on console errors, so it doubles as a smoke test.
//   pnpm build && pnpm screenshots [name ...]
import { mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startSession } from './lib/shoot.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const outDir = resolve(root, '../docs/images');
mkdirSync(outDir, { recursive: true });

const SHOTS = {
  amp: { query: 'section=amp' },
  // 0.3.0's five pedals with Pearl on (Nacre, the start-up preset, has only the three it uses).
  pedals: { query: 'section=pedals&board=gate,tide,pearl,coral,ripple&set=pearlEnabled:1' },
  // Add pedal's picker over a board with room on it.
  'pedal-picker': {
    query: 'section=pedals&board=gate,pearl,ripple&set=pearlEnabled:1',
    async prepare(page) {
      await page.getByRole('button', { name: 'Add pedal' }).click();
      await page.getByRole('dialog', { name: 'Add a pedal' }).waitFor();
      await page.mouse.move(1199, 759);
      await page.waitForTimeout(300);
    },
  },
  eq: {
    query:
      'section=eq&set=eqLagoonBand2:-2,eqLagoonBand3:-1,eqLagoonBand5:1.5,eqLagoonBand7:2.5,eqLagoonBand8:1,eqLagoonLowCut:70,eqLagoonHighCut:11000',
  },
  // The 4x12 with both mics placed freely: A turned off axis near the dust cap, B round at
  // 7 o'clock and backed off, the baffle map showing them against the other speakers.
  cab: {
    query:
      'section=cab&preset=mariana&set=micAPosition:0.35,micAAzimuth:40,micAOffAxis:30,micBPosition:0.8,micBAzimuth:-150,micBDistance:0.45',
  },
  fx: { query: 'section=fx&preset=soft-focus&set=doublerEnabled:1' },
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
  abyss: { query: 'section=amp&preset=mariana' },
  capture: {
    query: 'section=amp',
    async prepare(page) {
      await page.getByRole('radio', { name: /^Capture/ }).click();
      await page.getByRole('button', { name: 'Load capture…' }).click();
      await page.getByRole('button', { name: 'Replace…' }).click();
      await page.getByRole('button', { name: 'Replace…' }).click();
      await page.waitForTimeout(300);
    },
  },
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
      // Audition the next preset, so the footer offers the way back to Nacre.
      await page.getByRole('button', { name: /Open preset browser/ }).click();
      await page.getByRole('combobox', { name: 'Search presets' }).press('ArrowDown');
      await page.getByRole('option', { name: /^Riptide, playing/ }).waitFor();
      await page.waitForTimeout(400);
      // Centring the playing row can leave the list cut mid-row; start it on a whole row.
      await page.getByRole('listbox', { name: 'Presets' }).evaluate((list) => {
        const scroller = list.parentElement;
        const top =
          scroller.getBoundingClientRect().top + parseFloat(getComputedStyle(scroller).paddingTop);
        const rows = [...list.querySelectorAll('[role="option"]')];
        const first = rows.find((row) => row.getBoundingClientRect().bottom > top + 1);
        if (first) scroller.scrollTop += first.getBoundingClientRect().top - top;
      });
      await page.waitForTimeout(100);
    },
  },
  updates: {
    query: 'section=amp&update=available',
    async prepare(page) {
      await page.getByRole('button', { name: /^Update available/ }).click();
      await page.waitForTimeout(300);
    },
  },
};

const wanted = process.argv.slice(2);
const names = wanted.length
  ? wanted
  : ['amp', 'pedals', 'eq', 'cab', 'fx', 'presets', 'tuner', 'intonation'];

const session = await startSession(root, { scale: 2 });
try {
  for (const name of names) {
    const shot = SHOTS[name];
    if (!shot) throw new Error(`Unknown screenshot "${name}"`);
    const page = await session.open(name, `fresh&${shot.query}`, shot.prepare);
    const file = resolve(outDir, `${name}.png`);
    await page.screenshot({ path: file });
    console.log(`saved ${file}`);
    await page.close();
  }
} finally {
  await session.close();
}

if (session.problems.length) {
  console.error(session.problems.join('\n'));
  process.exit(1);
}
