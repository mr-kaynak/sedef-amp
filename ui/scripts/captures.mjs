// SPDX-License-Identifier: AGPL-3.0-or-later
// Still frames of real UI states for the product film: the 1200x760 canvas at 3x (3600x2280),
// rendered from dist/ against the frozen mock. Every state comes from URL flags (?set=, ?preset=)
// or the ?demo hook, so frames in a sequence differ only in what the sequence animates.
//   pnpm build && pnpm captures [--out <dir>] [name ...]
// Default output: <repo>/media/trailer/assets/shots. The lacquer grain is turned off (?grain=0):
// it is invisible at video resolution and makes every PNG several times larger.
import { mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeBrandAssets } from './brand-assets.mjs';
import { startSession } from './lib/shoot.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const outFlag = args.indexOf('--out');
const outDir = resolve(
  outFlag >= 0 ? args[outFlag + 1] : resolve(root, '../media/trailer/assets/shots'),
);
const wanted = args.filter((a, i) => a !== '--out' && i !== outFlag + 1);
mkdirSync(outDir, { recursive: true });

const BASE = 'fresh&freeze&demo&grain=0&host=standalone';
const PEDALS_OFF = 'tideEnabled:0,pearlEnabled:0,coralEnabled:0,rippleEnabled:0';
const LOW_E = 40;
const OPEN_E_HZ = 440 * Math.pow(2, (LOW_E - 69) / 12);

const settle =
  (ms = 700) =>
  async (page) =>
    page.waitForTimeout(ms);

async function startMeasuring(page) {
  await page.getByRole('button', { name: 'Start measuring' }).click();
  await page.getByRole('heading', { name: /^String 6/ }).waitFor();
}

const result = (cents) => ({
  openHz: OPEN_E_HZ,
  frettedHz: 2 * OPEN_E_HZ * Math.pow(2, cents / 1200),
  cents,
  stdCents: 0.06,
  measuredAt: 0,
});

/** name -> [query, prepare?] */
const SHOTS = {
  'pedals-off': [`section=pedals&set=${PEDALS_OFF}`],
  'pedals-pearl-on': [`section=pedals&set=${PEDALS_OFF},pearlEnabled:1`],
  ...Object.fromEntries(
    [0.2, 0.4, 0.6, 0.8].map((gain, i) => [
      `pearl-gain-${i + 1}`,
      [`section=pedals&set=${PEDALS_OFF},pearlEnabled:1,pearlGain:${gain}`],
    ]),
  ),
  'amp-lagoon': ['section=amp&set=ampModel:0'],
  'amp-reef': ['section=amp&set=ampModel:1'],
  'amp-abyss': ['section=amp&set=ampModel:2'],
  ...Object.fromEntries(
    [0.1, 0.3, 0.5, 0.7, 0.9].map((position, i) => [
      `cab-mic-${i + 1}`,
      [`section=cab&set=ampCabLink:0,cabModel:2,micAPosition:${position},micADistance:0.15`],
    ]),
  ),
  fx: ['section=fx&set=driftEnabled:1,driftHeads:2,fathomEnabled:1'],
  ...Object.fromEntries(
    [-18, -7, -1.5, 0].map((cents, i) => [
      `tuner-${i + 1}`,
      [
        'overlay=tuner',
        async (page) => {
          await page.evaluate(([midi, c]) => window.__sedefDemo.pitch(midi, c), [LOW_E, cents]);
          await page.waitForTimeout(900);
        },
      ],
    ]),
  ),
  'inton-open': [
    'overlay=intonation',
    async (page) => {
      await startMeasuring(page);
      await page.evaluate((midi) => {
        window.__sedefDemo.pitch(midi, 0.4);
        window.__sedefDemo.intonation({ pluck: 'measuring', tuneClose: true, tuneProgress: 0.64 });
      }, LOW_E);
      await page.waitForTimeout(900);
    },
  ],
  'inton-capture': [
    'overlay=intonation',
    async (page) => {
      await startMeasuring(page);
      await page.evaluate(
        ([openHz, midi]) => {
          window.__sedefDemo.intonation({ step: 'fret', openHz, tuneProgress: 1 });
          window.__sedefDemo.pitch(midi, 3.8);
          // After the reading, which starts the capture hold: the ring at 70 % of it.
          window.__sedefDemo.intonation({ pluck: 'measuring', captureProgress: 0.7 });
        },
        [OPEN_E_HZ, LOW_E + 12],
      );
      await page.waitForTimeout(900);
    },
  ],
  'inton-result': [
    'overlay=intonation',
    async (page) => {
      await startMeasuring(page);
      await page.evaluate(
        ([openHz, r]) =>
          window.__sedefDemo.intonation({
            step: 'result',
            openHz,
            captureProgress: 1,
            results: [r, null, null, null, null, null],
          }),
        [OPEN_E_HZ, result(3.8)],
      );
      await page.getByText('Octave error at the 12th fret').waitFor();
      await settle(400)(page);
    },
  ],
  'inton-summary': [
    'overlay=intonation',
    async (page) => {
      await page.evaluate(
        (results) => window.__sedefDemo.intonation({ phase: 'summary', results }),
        [3.8, -2.1, 0.6, 1.2, 4.4, -1.7].map(result),
      );
      await page.getByRole('button', { name: 'Copy as text' }).waitFor();
      await settle(400)(page);
    },
  ],
};

const names = wanted.length ? wanted : Object.keys(SHOTS);
const session = await startSession(root, { scale: 3 });
try {
  for (const name of names) {
    const shot = SHOTS[name];
    if (!shot) throw new Error(`Unknown capture "${name}"`);
    const [query, prepare = settle()] = shot;
    const page = await session.open(name, `${BASE}&${query}`, prepare);
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

if (!wanted.length) {
  const assets = await writeBrandAssets(resolve(outDir, '..'));
  console.log(`brand assets written to ${assets}`);
}
