#!/usr/bin/env python3
"""
Generate the PION/1 quarter-sine table and verify that it joins cleanly.

The PION/1 oscillator is a *bit* oscillator: the waveform is stored as a
2048-bit quarter cycle, not as floating point samples.  A full cycle is
therefore 8192 bits long, and the base frequency is

    f_base = fs / 8192          (61.04 Hz at fs = 500 kHz)

The table is produced by running a first-order delta-sigma modulator over one
quarter cycle of a sine.  A first-order DSM produces a 1-bit stream whose
running average tracks the input, which is exactly what we want: the "level"
of the oscillator lives in the *density of ones*, not in any amplitude field.

The quarter cycle is then mirrored and inverted to obtain the full cycle:

    Q[0..2047]            quadrant 0, rising
    mirror(Q)             quadrant 1, falling
    ~Q                    quadrant 2, rising (inverted)
    ~mirror(Q)            quadrant 3, falling (inverted)

The interesting failure mode is a *click* at the three joins: if the density
discontinuously jumps at a join, the reconstructed waveform has a step
artefact.  This script measures that discontinuity and fails if it is above a
threshold.

Usage:
    python3 tools/gen_sine_table.py                 # write src/dsp/sine_table.c
    python3 tools/gen_sine_table.py --check         # verify only
    python3 tools/gen_sine_table.py --out path.c
"""

import argparse
import math
import os
import sys

# Table geometry.
QUARTER_BITS = 2048          # bits in a quarter cycle
QUARTER_BYTES = QUARTER_BITS // 8
FULL_BITS = QUARTER_BITS * 4
FULL_BYTES = QUARTER_BITS // 2

# Delta-sigma modulator geometry.  We run the modulator over one quarter
# cycle at this many steps per bit-table-bit; each modulator step emits one
# bit of the table.
DSM_STEPS = QUARTER_BITS

# Accumulator scale.  The DSM accumulates the input in fixed point; the
# accumulator is allowed to swing over one full-scale period.
ACC_SCALE = 1 << 24

# Join-click threshold, in density units (1.0 == full scale).  The quarter
# sine goes 0 -> 1, so a "perfect" join has zero density discontinuity.
JOIN_CLICK_LIMIT = 0.02      # 2% of full scale


def quarter_sine_bits(steps=DSM_STEPS):
    """Run the first-order DSM over one quarter cycle of a sine.

    Returns a list of `steps` bits (0/1).
    """
    acc = 0
    bits = []
    for i in range(steps):
        # Phase 0 .. pi/2 inclusive-ish; end exactly at pi/2 so the mirror
        # join lands on the peak of the sine.
        theta = (math.pi / 2.0) * i / (steps - 1)
        x = math.sin(theta)                  # 0 .. 1
        # Scale into the accumulator's full-scale range (-1 .. +1).
        x_scaled = int(round(x * ACC_SCALE))
        acc += x_scaled
        # One-bit quantiser with a symmetric output level of +-1.
        half = ACC_SCALE
        if acc >= half:
            bits.append(1)
            acc -= 2 * half
        elif acc <= -half:
            bits.append(0)
            acc += 2 * half
        else:
            # Inside the dead band: emit the bit that reduces the error,
            # which for a symmetric quantiser means "keep the sign of acc".
            bits.append(1 if acc >= 0 else 0)
            if bits[-1]:
                acc -= 2 * half
            else:
                acc += 2 * half
    return bits


def build_full_cycle(quarter):
    """Mirror + invert the quarter cycle into a full 8192-bit cycle."""
    mirror = quarter[::-1]
    inv = [1 - b for b in quarter]
    inv_mirror = [1 - b for b in mirror]
    return quarter + mirror + inv + inv_mirror


def density(bits, start, length):
    """Mean density (0..1) of `length` bits starting at `start`."""
    n = len(bits)
    total = 0
    for k in range(length):
        total += bits[(start + k) % n]
    return total / length


def analyse_join_clicks(full_bits, win=64):
    """Measure the density discontinuity at the three quadrant joins.

    For each join we compare the density of a window that ends just before
    the join with the density of a window that starts just after it.  A
    continuous waveform gives a small difference; a click gives a large one.
    """
    results = []
    for j, name in ((QUARTER_BITS, "Q0/Q1"),
                    (2 * QUARTER_BITS, "Q1/Q2"),
                    (3 * QUARTER_BITS, "Q2/Q3")):
        before = density(full_bits, j - win, win)
        after = density(full_bits, j, win)
        results.append((name, before, after, abs(after - before)))
    return results


def worst_join_slope(full_bits, win=64):
    """Largest single-window density step anywhere in the cycle.

    This catches clicks that are not exactly at a quadrant boundary (e.g. a
    modulator glitch in the middle of the quarter).
    """
    worst = 0.0
    worst_at = 0
    n = len(full_bits)
    # Sliding one bit at a time is O(n*win); use a running sum instead.
    cur = sum(full_bits[:win])
    for i in range(n):
        nxt = cur - full_bits[i] + full_bits[(i + win) % n]
        step = abs(nxt - cur) / win
        if step > worst:
            worst = step
            worst_at = i
        cur = nxt
    return worst, worst_at


def pack_words(bits):
    """Pack an LSB-first bit list into 32-bit words (bit 0 == earliest)."""
    words = []
    for i in range(0, len(bits), 32):
        chunk = bits[i:i + 32]
        w = 0
        for k, b in enumerate(chunk):
            if b:
                w |= 1 << k
        words.append(w)
    return words


def c_array(words, per_line=4, indent="    "):
    out = []
    for i in range(0, len(words), per_line):
        row = ", ".join("0x%08xu" % w for w in words[i:i + per_line])
        out.append(indent + row + ",")
    return "\n".join(out)


def emit_c(quarter_words, full_words, stats):
    """Render the generated table as a C source file."""
    lines = []
    lines.append("/*")
    lines.append(" * PION/1 -- 1-bit quarter-sine table.   GENERATED FILE, DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Regenerate with:  python3 tools/gen_sine_table.py")
    lines.append(" *")
    lines.append(" * The table holds a quarter cycle of a sine as a 2048-bit 1-bit")
    lines.append(" * stream (64 words).  A full cycle is 8192 bits, so the base")
    lines.append(" * frequency is fs/8192 = 61.04 Hz at fs = 500 kHz.")
    lines.append(" *")
    lines.append(" * Bit 0 of each word is the EARLIEST sample (LSB-first), matching")
    lines.append(" * the PIO shift direction used everywhere else in the project.")
    lines.append(" */")
    lines.append('#include "sine_table.h"')
    lines.append("")
    lines.append("/* One quarter cycle, 2048 bits, LSB-first. */")
    lines.append("const uint32_t pion1_sine_quarter[%d] = {" % len(quarter_words))
    lines.append(c_array(quarter_words))
    lines.append("};")
    lines.append("")
    lines.append("/* Full cycle, 8192 bits, LSB-first: Q, mirror(Q), ~Q, ~mirror(Q). */")
    lines.append("const uint32_t pion1_sine_full[%d] = {" % len(full_words))
    lines.append(c_array(full_words))
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.join(here, os.pardir, "src", "dsp", "sine_table.c")
    ap.add_argument("--out", default=os.path.normpath(default_out),
                    help="path of the C file to write")
    ap.add_argument("--check", action="store_true",
                    help="only run the checks, do not write the file")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    quarter = quarter_sine_bits()
    full = build_full_cycle(quarter)

    joins = analyse_join_clicks(full)
    worst, worst_at = worst_join_slope(full)

    if not args.quiet:
        print("quarter sine: %d bits, density %.4f" %
              (len(quarter), sum(quarter) / len(quarter)))
        print("full cycle  : %d bits, density %.4f (want 0.5000)" %
              (len(full), sum(full) / len(full)))
        print()
        print("join clicks (window = 64 bits):")
        ok = True
        for name, before, after, delta in joins:
            flag = "OK " if delta <= JOIN_CLICK_LIMIT else "BAD"
            if delta > JOIN_CLICK_LIMIT:
                ok = False
            print("  %-8s before=%.4f after=%.4f  delta=%.4f  [%s]" %
                  (name, before, after, delta, flag))
        print()
        print("worst sliding density step: %.4f at bit %d" % (worst, worst_at))

        # Spectral sanity: the fundamental of the reconstructed full cycle
        # should sit at bin 1 of an 8192-point DFT.
        recon = []
        win = 64
        for i in range(len(full)):
            recon.append(density(full, i - win // 2, win))
        harm = []
        for k in (1, 2, 3):
            re = im = 0.0
            for i, x in enumerate(recon):
                a = 2.0 * math.pi * k * i / len(recon)
                re += x * math.cos(a)
                im += x * math.sin(a)
            harm.append((k, math.hypot(re, im) / len(recon)))
        print("reconstructed harmonics:")
        for k, mag in harm:
            print("  bin %d: %.4f" % (k, mag))

        if not ok:
            print()
            print("FAIL: join click above %.3f" % JOIN_CLICK_LIMIT)
            return 1
        print()
        print("PASS: all joins within %.3f" % JOIN_CLICK_LIMIT)

    if args.check:
        return 0

    quarter_words = pack_words(quarter)
    full_words = pack_words(full)
    src = emit_c(quarter_words, full_words, None)
    with open(args.out, "w") as f:
        f.write(src)
    if not args.quiet:
        print("wrote %s (%d + %d words)" %
              (args.out, len(quarter_words), len(full_words)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
