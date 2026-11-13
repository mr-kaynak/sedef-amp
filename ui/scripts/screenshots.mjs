// SPDX-License-Identifier: AGPL-3.0-or-later
// Renders the built UI (vite preview) against the mock backend and saves screenshots to
// docs/images at 1200x760 @2x; Sedef MIDI's ("midi") at its window's 760x600 @2x. Also fails on
// console errors, so it doubles as a smoke test.
//   pnpm build && pnpm build:midi && pnpm screenshots [--out <dir>] [name ...]
import { mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startSession } from './lib/shoot.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const outAt = args.indexOf('--out');
const outDir = resolve(outAt >= 0 ? (args[outAt + 1] ?? '') : resolve(root, '../docs/images'));
if (outAt >= 0) args.splice(outAt, 2);
mkdirSync(outDir, { recursive: true });

// Boards of three sizes for the pedalboard's layout: a factory preset's few, a busy one, and
// every pedal there is (the legacy order).
const BOARD_8 = 'gate,trench,conch,tide,pearl,kelp,gyre,ripple';
const BOARD_16 =
  'gate,trench,anemone,conch,remora,tide,urchin,sargasso,pearl,kelp,coral,beacon,gyre,jetty,ripple,breaker';

// Sedef MIDI's own build and page, at its window's size.
const MIDI = {
  configFile: resolve(root, 'vite.midi.config.ts'),
  page: 'midi.html',
  canvas: { width: 760, height: 600 },
};

const SHOTS = {
  // The standalone app, the demo bass line held on a sounding note.
  midi: { app: MIDI, query: 'host=standalone&frame=1450' },
  // The same with a long audio buffer and a low string heard (not a docs image: a layout check).
  'midi-long-buffer': { app: MIDI, query: 'host=standalone&frame=1450&buffer=512&heard=3' },
  amp: { query: 'section=amp' },
  // 0.3.0's five pedals with Pearl on (Glassy Chorus, the start-up preset, has only the three it uses).
  pedals: { query: 'section=pedals&board=gate,tide,pearl,coral,ripple&set=pearlEnabled:1' },
  // Add pedal's shelf under a board with room on it, pointing at Kelp: the bar on the board
  // shows where it would land.
  'pedal-picker': {
    query: 'section=pedals&board=gate,pearl,ripple&set=pearlEnabled:1',
    keepPointer: true,
    async prepare(page) {
      await page.getByRole('button', { name: 'Add pedal' }).click();
      await page.getByRole('dialog', { name: 'Add a pedal' }).waitFor();
      await page.getByRole('button', { name: 'Add Kelp, mid overdrive' }).hover();
      await page.waitForTimeout(400);
    },
  },
  'board-3': { query: 'section=pedals&board=gate,pearl,ripple&set=pearlEnabled:1' },
  'board-8': { query: `section=pedals&board=${BOARD_8}&set=pearlEnabled:1,tideEnabled:1` },
  'board-16': { query: `section=pedals&board=${BOARD_16}&set=pearlEnabled:1,tideEnabled:1` },
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
  // The metronome playing drums: the groove list for 4/4, intensity, fills, level and tone.
  drums: {
    query: 'section=amp&set=metronomeMode:1,drumsGroove:1',
    async prepare(page) {
      await page.getByRole('button', { name: 'Metronome' }).click();
      await page.getByRole('button', { name: 'Start drums' }).click();
      await page.waitForTimeout(700);
    },
  },
  // The groove list reached by keyboard: the groove that plays wears the focus ring (not a docs
  // image: a focus check).
  'drums-focus': {
    query: 'section=amp&set=metronomeMode:1,drumsGroove:1',
    async prepare(page) {
      await page.getByRole('button', { name: 'Metronome' }).click();
      await page.getByRole('combobox', { name: 'Metronome Signature' }).focus();
      await page.keyboard.press('Tab');
      await page.waitForTimeout(300);
    },
  },
  abyss: { query: 'section=amp&preset=mariana' },
  // Transpose two semitones down: the stepper at the end of the tab row, the latency it adds,
  // and the status bar's share.
  transpose: { query: 'section=pedals&set=transpose:-2' },
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
      // Audition the next preset, so the action row offers the way back to the first one.
      await page.getByRole('button', { name: /Open preset browser/ }).click();
      await page.getByRole('combobox', { name: 'Search presets' }).press('ArrowDown');
      await page.getByRole('option', { name: /^Jazz Warm, playing/ }).waitFor();
      await page.waitForTimeout(400);
    },
  },
  // The input level panel a few strums in: the meter with its zone, the verdict and what to do,
  // the Input knob and Audio settings.
  'input-level': {
    query: 'section=amp',
    async prepare(page) {
      await page.waitForTimeout(2500);
      await page.getByRole('button', { name: /, input level$/ }).click();
      await page.waitForTimeout(400);
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

const wanted = args;
const names = wanted.length
  ? wanted
  : [
      'amp',
      'pedals',
      'eq',
      'cab',
      'fx',
      'presets',
      'tuner',
      'intonation',
      'input-level',
      'transpose',
      'drums',
      'midi',
    ];

for (const name of names) if (!SHOTS[name]) throw new Error(`Unknown screenshot "${name}"`);

const problems = [];
// One session per build: Sedef Amp's, then Sedef MIDI's.
for (const app of [undefined, MIDI]) {
  const batch = names.filter((name) => SHOTS[name].app === app);
  if (!batch.length) continue;
  const session = await startSession(root, { scale: 2, ...(app ?? {}) });
  try {
    for (const name of batch) {
      const shot = SHOTS[name];
      const page = await session.open(name, `fresh&${shot.query}`, shot.prepare, {
        keepPointer: shot.keepPointer,
      });
      const file = resolve(outDir, `${name}.png`);
      await page.screenshot({ path: file });
      console.log(`saved ${file}`);
      await page.close();
    }
  } finally {
    await session.close();
  }
  problems.push(...session.problems);
}

if (problems.length) {
  console.error(problems.join('\n'));
  process.exit(1);
}
