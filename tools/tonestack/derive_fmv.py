# SPDX-License-Identifier: AGPL-3.0-or-later
"""Symbolic derivation of the three-knob passive ("FMV") tone stack transfer function.

Writes the nodal equations of the network drawn in engine/include/sedef/dsp/amp/ToneStack.h,
solves them with sympy and prints H(s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3)
with every coefficient collected in the pot positions t (treble), m (mid), l (bass). The result
matches eq. 3 of D. T. Yeh and J. O. Smith, DAFx-06, which ToneStack.cpp implements.

    uv run --with sympy python3 -I tools/tonestack/derive_fmv.py
"""

import sympy as sp

s, t, m, l = sp.symbols("s t m l")
C1, C2, C3, R1, R2, R3, R4 = sp.symbols("C1 C2 C3 R1 R2 R3 R4", positive=True)
vin = sp.Integer(1)
A, B, C, W, D = sp.symbols("A B C W D")  # node voltages; the output is the treble wiper

# Kirchhoff current law at each node (current in = current out). The unloaded treble wiper
# divides A..B, so it does not need a node of its own.
equations = [
    sp.Eq(s * C1 * (vin - A), (A - B) / R1),
    sp.Eq((A - B) / R1 + s * C2 * (D - B), (B - C) / (l * R2)),
    sp.Eq((B - C) / (l * R2), (C - W) / ((1 - m) * R3)),
    sp.Eq((C - W) / ((1 - m) * R3) + s * C3 * (D - W), W / (m * R3)),
    sp.Eq((vin - D) / R4, s * C2 * (D - B) + s * C3 * (D - W)),
]
solution = sp.solve(equations, [A, B, C, W, D], dict=True)[0]
h = sp.together(solution[B] + t * (solution[A] - solution[B]))
num, den = (sp.Poly(sp.expand(p), s) for p in sp.fraction(h))
a0 = den.coeff_monomial(1)

for name, poly in (("b", num), ("a", den)):
    for k in range(4):
        coeff = sp.expand(sp.simplify(poly.coeff_monomial(s**k) / a0))
        print(f"{name}{k} = {sp.collect(coeff, [t, m, l])}")
