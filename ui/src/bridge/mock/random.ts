// SPDX-License-Identifier: AGPL-3.0-or-later
/** Small deterministic PRNG (mulberry32) so simulations are repeatable in tests. */
export function createRandom(seed = 0x5edef): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Approximately normal noise (Irwin–Hall with 4 terms), unit variance. */
export function gaussian(random: () => number): number {
  return (random() + random() + random() + random() - 2) * Math.sqrt(3);
}
