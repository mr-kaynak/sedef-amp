#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""How long must one channel be observed to tell which strings sound?

The numbers behind docs/design/guitar-to-midi-chords.md, "Identifiability and time". Each string
is a set of partials k f0 sqrt(1 + B k^2) with free complex amplitudes; the observation is T
seconds of their sum. For a note m and a set of other partials, eta is the fraction of m's energy
(nominal amplitudes 1/k, random phases averaged) that lies outside the span of the other partials
over that window: what is left to see of m once the others are allowed to explain all they can.

    python3 tools/poly/identifiability.py

Needs numpy only. Deterministic.
"""
import numpy as np

FS = 12000.0          # the partials that matter lie under 5 kHz
TOP_HZ = 4000.0       # partials above this are not used (weak, and the pick transient lives there)
B_OPEN = 1.0e-4       # stiffness of an open string, the repo's string model (PitchTestSignals.h)


def hz(pitch):
    return 440.0 * 2.0 ** ((pitch - 69) / 12.0)


def partials(pitch, b=B_OPEN, detune_cents=0.0):
    f0 = hz(pitch) * 2.0 ** (detune_cents / 1200.0)
    out = []
    k = 1
    while True:
        f = k * f0 * np.sqrt(1.0 + b * k * k)
        if f > TOP_HZ:
            break
        out.append((f, 1.0 / k))
        k += 1
    return out


def basis(freqs, t):
    cols = []
    for f in freqs:
        cols.append(np.cos(2 * np.pi * f * t))
        cols.append(np.sin(2 * np.pi * f * t))
    return np.array(cols).T


def eta(note, others, seconds):
    """Fraction of `note`'s energy outside the span of `others` over a window of `seconds`."""
    n = int(round(seconds * FS))
    t = (np.arange(n) - 0.5 * (n - 1)) / FS
    if others:
        q, _ = np.linalg.qr(basis([f for f, _ in others], t))
    num = den = 0.0
    for f, a in note:
        for col in (np.cos(2 * np.pi * f * t), np.sin(2 * np.pi * f * t)):
            rest = col - q @ (q.T @ col) if others else col
            num += a * a * float(rest @ rest)
            den += a * a * float(col @ col)
    return num / den


def time_for(note, others, need, grid):
    for seconds in grid:
        if eta(note, others, seconds) >= need:
            return seconds
    return None


GRID = [0.005 * k for k in range(1, 41)] + [0.25, 0.3, 0.4, 0.5, 0.75, 1.0]


def fmt(seconds):
    return "  >1 s" if seconds is None else "%4.0f ms" % (1000.0 * seconds)


def main():
    print("1. Which of two notes a semitone apart: time until eta >= need, the note alone")
    print("   (its neighbour's partials may explain what they can)")
    print("   note    f0 Hz   need 0.05   need 0.10   need 0.25   need 0.50   periods at 0.10")
    for pitch, name in [(28, "E1"), (33, "A1"), (40, "E2"), (45, "A2"), (50, "D3"), (55, "G3"),
                        (59, "B3"), (64, "E4"), (76, "E5")]:
        row = [time_for(partials(pitch), partials(pitch + 1), need, GRID)
               for need in (0.05, 0.10, 0.25, 0.50)]
        print("   %-4s  %7.1f   %s    %s    %s    %s    %5.1f" % (
            name, hz(pitch), fmt(row[0]), fmt(row[1]), fmt(row[2]), fmt(row[3]),
            row[1] * hz(pitch) if row[1] else float("nan")))

    print()
    print("2. Is the note there at all, next to the rest of the chord: eta after T")
    shapes = [
        ("E", [40, 47, 52, 56, 59, 64]),
        ("Am", [45, 52, 57, 60, 64]),
        ("D", [50, 57, 62, 66]),
        ("G", [43, 47, 50, 55, 59, 67]),
        ("C", [48, 52, 55, 60, 64]),
        ("F barre", [41, 48, 53, 57, 60, 65]),
        ("E5", [40, 47, 52]),
        ("A5", [45, 52, 57]),
        ("E2+F2", [40, 41]),
        ("E2+B2 fifth", [40, 47]),
        ("E2+E3 octave", [40, 52]),
        ("E2+G#4", [40, 68]),
    ]
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    for label, shape in shapes:
        print("   %s" % label)
        print("     note    20 ms   40 ms   60 ms   80 ms  120 ms  200 ms  500 ms   time to 0.10")
        for m in shape:
            # A fretted note's string is shorter: B grows as 2^(fret / 6). Strings differ; a
            # third of a semitone of detuning between them stands for that and for tuning.
            others = []
            for j, o in enumerate(shape):
                if o != m:
                    others += partials(o, B_OPEN * (1.0 + 0.5 * (j % 3)), 2.0 * ((j % 3) - 1))
            mine = partials(m)
            row = [eta(mine, others, s) for s in (0.02, 0.04, 0.06, 0.08, 0.12, 0.2, 0.5)]
            print("     %-3s%d  %s   %s" % (
                names[m % 12], m // 12 - 1, "  ".join("%6.3f" % v for v in row),
                fmt(time_for(mine, others, 0.10, GRID))))

    print()
    print("3. An octave above a sounding string: beat between partial 2k below and k above,")
    print("   k^3 f0 (4 B1 - B2), and the time 1/beat to resolve it (B1 = B2 = %.0e)" % B_OPEN)
    for pitch, name in [(40, "E2+E3"), (45, "A2+A3"), (52, "E3+E4")]:
        f0 = hz(pitch)
        print("   %s: " % name + ", ".join(
            "k=%d %.2f Hz (%.0f ms)" % (k, 3 * B_OPEN * k ** 3 * f0,
                                       1000.0 / (3 * B_OPEN * k ** 3 * f0))
            for k in (1, 2, 3, 4, 6)))


if __name__ == "__main__":
    main()
