# PION/1

**A one-bit oversampled synthesizer on the Raspberry Pi Pico.**

PION/1 is a musical instrument that lives entirely in the 1-bit domain. There
is no 16-bit sample, no 24-bit accumulator, no floating point anywhere in the
signal path. Every voice produces a stream of bits, every control parameter
is a number of *slots*, and the analog front end reconstructs the waveform
from nothing but the density of ones in that stream.

It runs on an RP2040 at a bit rate of exactly **500 kHz**.

---

## Why 1-bit?

Because it is interesting, and because it turns out to be a surprisingly good
way to build a synthesizer.

A 1-bit signal has no amplitude information at all — only a *density*. The
level of the output is the proportion of bits that are set over time. That
constraint is brutal in the digital domain and completely natural in the
analog one: the comparator and the flip-flop that make up the input stage are
1-bit devices anyway, so working in bits means the software and the hardware
agree on the representation instead of fighting over it.

The consequences ripple through the whole design:

| Concern | 16-bit world | PION/1 |
|---|---|---|
| A voice's amplitude | a float or fixed-point field | the number of slots it owns in a 32-bit word |
| Mixing | add, then clamp | bit-steal: louder voices take slots from quieter ones |
| Oscillator | a lookup table of amplitudes | a 2048-bit quarter-cycle of a sine |
| Filtering | multiply and accumulate | count the ones in a window |

None of this is a limitation in disguise. It is a different set of tools, and
it produces a sound with a character you cannot easily get otherwise.

---

## Architecture

```
                 ┌──────────────────────────────────────────┐
                 │              RP2040 (Pico)               │
                 │                                          │
   analog in ────▶│  comparator ─▶ D flip-flop ─▶ PIO SM0    │──▶ capture ring
                 │                   ▲            (500 kHz) │    (512 words)
                 │                   │                       │
                 │              side-set clock               │
                 │                   │                       │
                 │  ┌────────────────┴─────────────────┐     │
                 │  │             core 1 (DSP)          │     │
                 │  │  sampler ─┐                      │     │
                 │  │  osc ─────┼──▶ mask-select mixer │     │
                 │  │  waveguide┘        (32 slots)    │     │
                 │  └────────────────┬─────────────────┘     │
                 │                   │                       │
                 │  ┌────────────────┴─────────────────┐     │
                 │  │             core 0 (control)     │     │
                 │  │  parameters, UI, housekeeping    │     │
                 │  └──────────────────────────────────┘     │
                 │                                          │
                 │  PIO SM1 ◀── playback ring ◀── mixer      │──▶ analog out
                 │  (500 kHz)                                │
                 └──────────────────────────────────────────┘
```

### The bit rate

Everything is derived from one number: **500 000 bits per second**, which is
one bit every 2 µs. The RP2040 runs at 125 MHz, so a PIO state machine
clocked with `clkdiv = 250` executes exactly one instruction per bit.

That gives the project its natural quantum: **a 32-bit word is 64 µs of
audio**, and the word rate is 15 625 words per second.

### The two cores

* **Core 1 (DSP)** renders one word every 64 µs and pushes it into the
  playback ring. It has a hard deadline, so nothing else is allowed to run
  on it.
* **Core 0 (control)** owns the parameters, the console UI and the
  housekeeping. It publishes parameter updates through a lock-free
  single-writer block.

The split is not decoration: a USB enumeration or a `printf` on core 0 would
otherwise cause an audible dropout.

---

## The pieces

### The sine table is a bit stream

`tools/gen_sine_table.py` generates the oscillator's waveform by running a
**first-order delta-sigma modulator** over a quarter cycle of a sine. The
result is 2048 bits — 64 words — stored LSB-first.

A full cycle is therefore 8192 bits, and the base frequency is
`fs / 8192` = **61.04 Hz**. A frequency of `f` Hz is a phase increment of
`f × 2³² / fs` per sample.

The quarter table is mirrored and inverted to make the full cycle:

```
Q[0..2047]        rising
mirror(Q)         falling
~Q                rising, inverted
~mirror(Q)        falling, inverted
```

The interesting failure mode is a **click** at the three joins, where the
density of the stream steps discontinuously. The generator measures that
discontinuity and fails the build if it exceeds 2 % of full scale. It also
verifies that the reconstructed waveform is a *pure* sine: the harmonics
must sit below 5 % of the fundamental.

Regenerate with:

```sh
python3 tools/gen_sine_table.py
```

### The mixer is bit-stealing

Each output word has 32 slots. Each voice owns a disjoint set of them, and
the masks rotate right by one bit every word.

Three things fall out of that:

1. **Levels are slot counts.** A voice at 50 % owns 16 of the 32 slots. A
   louder voice takes slots from a quieter one; the slots nobody owns are
   filled with the idle pattern, which is the floor of the signal. This is
   ordinary bit-crusher stealing, and it is the only way to control the
   level of a 1-bit signal without an amplitude field.

2. **Voices never fight.** Because the masks are disjoint, the mix is a
   genuine sum rather than an overwrite.

3. **The rotation kills the aliasing.** A static mask makes each voice's
   contribution land in the same slot every word, which shows up as a
   harsh aliasing tone. Rotating the masks turns that tone into broadband
   noise, because the slot assignment drifts relative to the word boundary
   while the arrangement *between* the voices stays fixed.

The slots are distributed with a deficit scheduler so that each voice's bits
are spread evenly through the word. Clumped bits would reconstruct as a
low-frequency burst rather than as a steady partial.

### The sampler resamples in three tiers

Reading a captured bit stream at an arbitrary rate is the subtle part. Three
quality tiers are available:

* **CRUDE** — take the nearest source bit. Cheap, aliases badly.
* **BOXCAR** — count the ones in a sliding window of *L* bits, then
  re-quantise that count back to a bit with a first-order feedback loop. The
  window kills the aliases; the loop keeps the output a true 1-bit stream.
* **REMOD** — as above, but the window position is interpolated
  fractionally, so the decimation phase is not quantised to whole source
  bits.

The window count is maintained *incrementally*: the bits that leave the
window are subtracted and the bits that enter are added, so a long window is
nearly free when the read rate is close to 1.

The feedback loop is worth spelling out, because it is the heart of the
whole idea:

```
err += (count - L/2) - (out ? +L/2 : -L/2)
out  = (err >= 0)
```

In the count domain a full-scale window of `L` ones is `L`, so the output
bit contributes `±L/2` to the comparison — exactly like the window average
does. Averaged over time, the loop therefore drives the *density* of the
output bits towards the density of the input window. The level is
preserved; only the shape is quantised.

### The capture path

The input stage is a comparator into a D flip-flop, and the flip-flop is
clocked by a PIO side-set pin:

```
.program pion1_capture
.side_set 1
.wrap_target
    set x, 0 side 1     ; CLK high: rising edge latches D
    in pins, 1 side 0   ; CLK low, and sample the settled Q
```

The capture SM runs two instructions per bit at `clkdiv = 125` (1 MHz
instruction rate), which puts the sample exactly 1 µs after the clock edge.
That is far longer than a 74HC74's `t_co` (~40 ns) and than the PIO's
two-stage input synchroniser delay (~24 ns at 125 MHz). The whole loop
latency is one bit period, which is what the analog feedback path has to be
designed around.

The playback SM runs one instruction per bit at `clkdiv = 250`, with autopull
so the DMA can feed it a whole word at a time.

Both state machines are enabled in the same cycle with
`pio_enable_sm_mask_in_sync()`, so the capture and playback paths share one
timebase.

---

## Building

### Firmware

```sh
export PICO_SDK_PATH=~/pico-sdk
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The result is `build/PION1.uf2`, which you can drag onto the Pico in
BOOTSEL mode.

### Host tests

The DSP core has no Pico SDK dependencies, so it compiles and runs on the
host. This turns "it builds" into "it produces the right numbers", which is
the only way to be confident in a 1-bit signal chain without an oscilloscope
on the bench.

```sh
cmake -B tests/build -S tests -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build -j
./tests/build/test_dsp
```

The suite covers the bit primitives, the sine table (joins and harmonic
purity), the mixer (disjointness, rotation, level-to-slot mapping), the
resampler (density preservation in all three tiers) and the FM voice
(sidebands must appear at `fc ± n·fm`).

### Host renderer

The renderer produces a WAV file using exactly the same DSP code as the
firmware, so you can listen to what the hardware will produce before it
exists.

```sh
./tests/build/render_host --freq 440 --seconds 2 --out tone.wav
./tests/build/render_host --freq 440 --fm-freq 100 --fm-depth 200 --out fm.wav
```

The output is 16-bit mono at 44.1 kHz, decimated from the 500 kHz bit stream
by the same boxcar the analog output stage performs.

---

## Wiring

The default pin assignment:

| Pico pin | Function |
|---|---|
| GP0 | capture input (flip-flop Q) |
| GP1 | flip-flop clock (PIO side-set) |
| GP2 | playback output |
| GP4/GP5 | UART console fallback |

A minimal input stage needs a comparator (LM311, or an LM393 for single
supply), a 74HC74 flip-flop, and a feedback resistor to the summing node.
A minimal output stage needs an RC reconstruction filter followed by an
op-amp stage to bring the signal up to line level.

Full details, including the timing budget and a bill of materials, are in
[docs/HARDWARE.md](docs/HARDWARE.md).

---

## Running it

The console is USB CDC. On the host the port appears as a stable symlink,
so it does not matter what order things are plugged in:

```sh
tools/pion1_flash.sh                  # build, flash, wait for the console
python3 tools/pion1_console.py        # attach and print
```

`tools/pion1_flash.sh` waits for the Pico in BOOTSEL mode, copies the
UF2, and waits for the CDC to come back. If the Pico is running a build
without USB support it is invisible to the host, so hold the **BOOTSEL**
button while pressing **RESET** — the script takes it from there.

The firmware exposes the RP-series reset interface, so a running PION/1
can be dropped into the bootloader over USB instead:

```sh
picotool reboot -u -f
```

---

## Status

This is an early release. The signal chain is complete and verified by the
host test suite, but the following are still open:

* The console speaks USB CDC and greets you whenever you attach, but its UI
  is still a few keystrokes rather than a real interface.
* The waveguide voice is implemented but has not been tuned.
* The analog front end has not been built and measured on real hardware;
  the timing in `docs/HARDWARE.md` is calculated, not verified.
* The capture ring is written but the sampler does not yet track the DMA
  write position, so it reads the whole ring rather than the newest audio.

The parts that *are* finished — the sine table, the mixer, the resampler and
the FM voice — are the parts that are hardest to get right, and they are
covered by tests that check the actual signal rather than just the code
paths.

---

## Documentation

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the signal flows, and why
* [docs/HARDWARE.md](docs/HARDWARE.md) — analog front end, timing budget, BOM
* [docs/BITDOMAIN.md](docs/BITDOMAIN.md) — notes on working in 1-bit signals

The short version is also published as a web page at
<https://bezko.github.io/PION-1/>, deployed by CI from `docs/site/`.

---

## Licence

MIT. See [LICENSE](LICENSE).

---

## Repository location

The project was requested as `PION/1`. The `pion` GitHub organisation is
the [Pion WeRTC](https://github.com/pion) project — an unrelated Go
WebRTC stack — and this account has no admin rights there, so a repo could
not be created in it.

The repository therefore lives at **`bezko/PION-1`**. When admin access to
the intended organisation is available, it can be moved there with:

```sh
gh repo transfer bezko/PION-1 PION/PION-1
```

The `PION-1` name, the `PION/1` branding and the project structure are all
intended to carry over unchanged.
