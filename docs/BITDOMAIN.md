# Notes on working in the 1-bit domain

Some things that are obvious in the multi-bit world behave differently when
the only thing you have is a bit. These are the notes I would want if I
were starting this again.

---

## Level is density

There is no amplitude field. The level of a signal is the proportion of bits
that are set, measured over a window.

This has one immediate consequence: **you cannot change the level without
changing the timing or the width of the window over which it is measured.**
A "gain" control has to be implemented as something else — in PION/1 it is
implemented as the number of slots a voice owns.

## Averaging is filtering

The only filter available is a boxcar: count the ones in a window and
divide by the window length. That is a low-pass filter, and its cutoff is
`fs / (2L)` for a window of `L` bits.

Which means the window length is the single most important parameter in the
project. Too short and you get aliasing. Too long and you get latency, and
the noise band gets wider as the level drops.

## Multiplication is hard

A 1-bit signal times a 1-bit signal is a 1-bit signal, and the operation is
XOR. That is a poor multiplier, but it is a fine *adder* in the sense that
matters for noise shaping: XOR is linear over GF(2), so the first-order
DSM is just a one-bit accumulator.

## Aliasing is not a bug, it is the medium

A square wave at 440 Hz sampled at 500 kHz has harmonics at every odd
multiple: 440, 1320, 2200, 3080... all of which are below Nyquist, so they
survive. But the *bit* stream's discontinuities also generate energy at
frequencies above Nyquist, which fold back down.

Rotating the mixer masks every word is the standard mitigation: it moves
the aliasing energy around in time so it smears into noise instead of
landing on a pitch.

## Clicking is a density discontinuity

A click is a step in the reconstructed waveform. In the bit domain, a step
means the density changes abruptly at some instant.

The mirror-and-invert construction of the sine table is the obvious place
this can go wrong, which is why the generator measures it. The same
principle applies anywhere a bit stream is spliced: always check the
density on both sides of the splice.

## The feedback loop is the whole trick

Everything in PION/1 comes back to this loop:

```
err += (count - L/2) - (out ? +L/2 : -L/2)
out  = (err >= 0)
```

A first-order feedback loop that compares a *quantised* version of a signal
against the original, and integrates the error. This is the standard
delta-sigma idea, and it is why the output level is preserved exactly even
though every individual sample is only a bit.

If you ever find yourself wanting to control the level of a 1-bit signal,
this loop is the answer: it moves the error into the *timing* of the bits
rather than their value.

## Beware the overflow

The FM path in this project was wrong for a while because the modulation
depth was multiplied by the index in 32-bit arithmetic, and the product
overflowed silently. The symptom was subtle: the FM had no effect at all,
rather than being wrong in an obvious way.

Multiplying a 32-bit phase offset by a 16.16 index needs 64 bits. The same
applies to any level-scaled quantity in this project: use 64-bit
intermediates.

## Test the signal, not the code

A 1-bit signal chain has very few places where a bug produces an obvious
symptom. A wrong bit order, a slightly wrong window length, an off-by-one
in the phase accumulator — all of these produce a signal that still *sounds
like something*.

The tests in `tests/test_dsp.c` therefore measure things:

* the density of the output must match the density of the input;
* the harmonics of the sine table must be below 5 % of the fundamental;
* the FM sidebands must appear at exactly `fc ± n·fm`;
* the mixer masks must stay disjoint and must actually rotate.

Those four checks catch most of the bugs that a "does it compile" check
would miss.

## Keep the DSP free of the SDK

`src/dsp/` has no Pico SDK dependency at all. That is what lets the whole
signal chain be compiled and tested on the host machine, and it is the
single highest-value structural decision in the project.

If you add a `#include "pico/stdlib.h"` to a file in `src/dsp/`, you lose
the ability to run 55 assertions against the actual signal in under a
second. Please don't.
