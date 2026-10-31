// SPDX-License-Identifier: AGPL-3.0-or-later
// Vector brand assets for the product film, generated from the same sources the UI uses:
//   logo.svg      the eight-pointed nacre star (geometry from src/features/header/starMark.ts)
//   wordmark.svg  "Sedef" as the header sets it (Bodoni Moda italic, wght 500, opsz 28), outlined
//   lockup.svg    star + wordmark + "amp", as in the header
//   fonts/        the bundled woff2 files, their OFL licences and a fonts.css
// Variable WOFF2 is decompressed to TTF (wawoff2) so fontkit can instance the exact axes.
//   node scripts/brand-assets.mjs [assetsDir]   (default <repo>/media/trailer/assets)
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const require = createRequire(import.meta.url);
const fontkit = require('fontkit');
const wawoff2 = require('wawoff2');

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const fontsource = resolve(root, 'node_modules/@fontsource-variable');
const BONE = '#ede6d6';
const BONE_3 = '#86918f';

const round = (n) => Number(n.toFixed(2));

async function instance(file, axes) {
  const ttf = Buffer.from(await wawoff2.decompress(readFileSync(file)));
  return fontkit.create(ttf).getVariation(axes);
}

/** Lays out text and returns one outlined path, positioned with its baseline at `baseline`. */
function outline(font, text, { size, x = 0, baseline, letterSpacingEm = 0 }) {
  const s = size / font.unitsPerEm;
  const run = font.layout(text);
  let pen = 0;
  const parts = [];
  const box = { minX: Infinity, minY: Infinity, maxX: -Infinity, maxY: -Infinity };
  run.glyphs.forEach((glyph, i) => {
    const pos = run.positions[i];
    const path = glyph.path.transform(
      s,
      0,
      0,
      -s,
      x + (pen + pos.xOffset) * s,
      baseline - pos.yOffset * s,
    );
    for (const c of path.commands) {
      const a = c.args.map(round);
      parts.push(
        { moveTo: 'M', lineTo: 'L', quadraticCurveTo: 'Q', bezierCurveTo: 'C', closePath: 'Z' }[
          c.command
        ] + a.join(' '),
      );
    }
    const b = path.bbox;
    if (Number.isFinite(b.minX)) {
      box.minX = Math.min(box.minX, b.minX);
      box.minY = Math.min(box.minY, b.minY);
      box.maxX = Math.max(box.maxX, b.maxX);
      box.maxY = Math.max(box.maxY, b.maxY);
    }
    pen += pos.xAdvance + letterSpacingEm * font.unitsPerEm;
  });
  return { d: parts.join(''), advance: pen * s, box };
}

function starSvg(
  STAR_POINTS,
  NACRE_STOPS,
  INK,
  { x = 0, y = 0, size = 40, idPrefix = 'sedef' } = {},
) {
  const k = size / 40;
  const stops = NACRE_STOPS.map(([o, c]) => `<stop offset="${o}" stop-color="${c}"/>`).join('');
  return {
    defs:
      `<linearGradient id="${idPrefix}-nacre" x1="0" y1="0" x2="1" y2="1">${stops}</linearGradient>` +
      `<radialGradient id="${idPrefix}-sheen" cx="0.34" cy="0.3" r="0.55">` +
      `<stop offset="0" stop-color="#fff" stop-opacity="0.8"/><stop offset="1" stop-color="#fff" stop-opacity="0"/>` +
      `</radialGradient>`,
    body:
      `<g transform="translate(${round(x)} ${round(y)}) scale(${round(k)})">` +
      `<polygon points="${STAR_POINTS}" fill="url(#${idPrefix}-nacre)"/>` +
      `<polygon points="${STAR_POINTS}" fill="url(#${idPrefix}-sheen)"/>` +
      `<circle cx="20" cy="20" r="2.4" fill="${INK}" fill-opacity="0.8"/></g>`,
  };
}

const svg = (w, h, title, defs, body, scale = 1) =>
  `<svg xmlns="http://www.w3.org/2000/svg" width="${round(w * scale)}" height="${round(h * scale)}" viewBox="0 0 ${round(w)} ${round(h)}">` +
  `<title>${title}</title>${defs ? `<defs>${defs}</defs>` : ''}${body}</svg>\n`;

/** Keeps the latin and latin-ext @font-face blocks of a fontsource stylesheet. */
function fontFaces(cssFile) {
  const css = readFileSync(cssFile, 'utf8');
  return [...css.matchAll(/\/\* ([^*]+) \*\/\s*@font-face \{[^}]+\}/g)]
    .filter((m) => /-latin(-ext)?-/.test(m[1]))
    .map((m) => m[0].replace(/url\(\.\/files\//g, 'url(./'));
}

export async function writeBrandAssets(assetsDir) {
  const { STAR_POINTS, NACRE_STOPS, INK } = await import(
    pathToFileURL(resolve(root, 'src/features/header/starMark.ts')).href
  );
  mkdirSync(resolve(assetsDir, 'fonts'), { recursive: true });

  // logo.svg
  const star = starSvg(STAR_POINTS, NACRE_STOPS, INK);
  writeFileSync(resolve(assetsDir, 'logo.svg'), svg(40, 40, 'Sedef', star.defs, star.body, 10));

  // wordmark.svg
  const bodoni = await instance(
    resolve(fontsource, 'bodoni-moda/files/bodoni-moda-latin-opsz-italic.woff2'),
    {
      wght: 500,
      opsz: 28,
    },
  );
  const archivo = await instance(
    resolve(fontsource, 'archivo/files/archivo-latin-wdth-normal.woff2'),
    {
      wght: 500,
      wdth: 125,
    },
  );
  const word = outline(bodoni, 'Sedef', { size: 100, baseline: 0, letterSpacingEm: -0.01 });
  const pad = 2;
  const ww = word.box.maxX - word.box.minX + 2 * pad;
  const wh = word.box.maxY - word.box.minY + 2 * pad;
  writeFileSync(
    resolve(assetsDir, 'wordmark.svg'),
    svg(
      ww,
      wh,
      'Sedef',
      '',
      `<path transform="translate(${round(pad - word.box.minX)} ${round(pad - word.box.minY)})" fill="${BONE}" d="${word.d}"/>`,
      4,
    ),
  );

  // lockup.svg: the header arrangement in CSS px (star 30, gap 10, Sedef 27px, gap 6, amp 11px).
  const baseline = 28;
  const name = outline(bodoni, 'Sedef', { size: 27, x: 40, baseline, letterSpacingEm: -0.01 });
  const sub = outline(archivo, 'amp', {
    size: 11,
    x: 40 + name.advance + 6,
    baseline,
    letterSpacingEm: 0.06,
  });
  const mark = starSvg(STAR_POINTS, NACRE_STOPS, INK, { x: 0, y: baseline - 9.5 - 15, size: 30 });
  const lw = 40 + name.advance + 6 + sub.advance + 2;
  writeFileSync(
    resolve(assetsDir, 'lockup.svg'),
    svg(
      lw,
      40,
      'Sedef amp',
      mark.defs,
      `${mark.body}<path fill="${BONE}" d="${name.d}"/><path fill="${BONE_3}" d="${sub.d}"/>`,
      8,
    ),
  );

  // fonts/
  const files = [
    'archivo/files/archivo-latin-wdth-normal.woff2',
    'archivo/files/archivo-latin-ext-wdth-normal.woff2',
    'bodoni-moda/files/bodoni-moda-latin-opsz-italic.woff2',
    'bodoni-moda/files/bodoni-moda-latin-ext-opsz-italic.woff2',
    'bodoni-moda/files/bodoni-moda-latin-opsz-normal.woff2',
    'bodoni-moda/files/bodoni-moda-latin-ext-opsz-normal.woff2',
  ];
  for (const f of files)
    copyFileSync(resolve(fontsource, f), resolve(assetsDir, 'fonts', f.split('/').pop()));
  copyFileSync(resolve(fontsource, 'archivo/LICENSE'), resolve(assetsDir, 'fonts/Archivo-OFL.txt'));
  copyFileSync(
    resolve(fontsource, 'bodoni-moda/LICENSE'),
    resolve(assetsDir, 'fonts/BodoniModa-OFL.txt'),
  );
  const faces = [
    ...fontFaces(resolve(fontsource, 'archivo/wdth.css')),
    ...fontFaces(resolve(fontsource, 'bodoni-moda/opsz-italic.css')),
    ...fontFaces(resolve(fontsource, 'bodoni-moda/opsz.css')),
  ];
  writeFileSync(
    resolve(assetsDir, 'fonts/fonts.css'),
    `/* The Sedef Amp UI fonts (SIL Open Font License 1.1, see the *-OFL.txt files).
   UI text: 'Archivo Variable' (panel legends use font-stretch 118-125%).
   Module names and the wordmark: 'Bodoni Moda Variable', italic, weight 500,
   font-variation-settings 'opsz' 28 (wordmark) to 96 (large names). */
${faces.join('\n\n')}
`,
  );
  return assetsDir;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const dir = resolve(process.argv[2] ?? resolve(root, '../media/trailer/assets'));
  await writeBrandAssets(dir);
  console.log(`brand assets written to ${dir}`);
}
