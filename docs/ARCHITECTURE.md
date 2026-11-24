# PION/1 — architecture

This document explains how the signal flows through PION/1 and why it is
built the way it is. The README covers the same ground at a higher level.

---

## The one number everything derives from

**500 000 bits per second.**

One bit every 2 µs. The RP2040 system clock is 125 MHz, so a PIO state
machine with `clkdiv = 250` executes one instruction per bit. Everything
else in the project is a multiple of that:

| Quantity | Value | Derivation |
|---|---|---|
| Bit period | 2 µs | 1 / 500 kHz |
| Word | 32 bits | the width of the PIO shift registers |
| Word period | 64 µs | 32 × 2 µs |
| Word rate | 15 625 Hz | 500 kHz / 32 |
| Capture ring | 32 ms | 512 words |
| Playback ring | 32 ms | 512 words |
| Sine base note | 61.04 Hz | 500 kHz / 8192 |

The capture and playback state machines use **different clock dividers** —
the capture SM runs two instructions per bit at `clkdiv = 125`, the playback
SM one instruction per bit at `clkdiv = 250`. Both therefore share the same
bit period, and both are started in the same cycle with
`pio_enable_sm_mask_in_sync()`, so they stay locked together indefinitely.
The dividers are exact integers, so there is no drift.

---

## Bit ordering

Throughout the project a **word is 32 bits, LSB-first**: bit 0 is the
*earliest* sample, bit 31 the latest.

This matches the RP2040 PIO shift direction used in `src/pio/pion1.pio`:

* the capture program uses `in pins, 1` with `shift_right = true`, so the
  first sampled bit ends up at bit 31 and the 32nd at bit 0;
* the playback program uses `out pins, 1` with `shift_right = true`, so bit
  0 is emitted first.

Because both directions agree, **a word captured from the input can be
replayed on the output with no bit reversal anywhere in the chain**. That
saves a whole class of bugs, and it is why the `pion1_popcnt_range()` helper
takes explicit bit offsets rather than assuming a particular alignment.

---

## The signal paths

### Capture (input)

```
analog in ─▶ comparator ─▶ D flip-flop ─▶ GP0 ─▶ PIO SM0 ─▶ DMA ─▶ capture ring
                              ▲
                              │ GP1, PIO side-set
```

The D flip-flop is the important part. It is clocked by a PIO side-set pin,
and the PIO samples its *output*, not the comparator's. That gives a clean
one-bit-per-period sample of a signal that is otherwise asynchronous.

The timing is explicit in the PIO program:

```pio
.program pion1_capture
.side_set 1
.wrap_target
    set x, 0 side 1     ; CLK high: rising edge latches D
    in pins, 1 side 0   ; CLK low, and sample the settled Q
```

At `clkdiv = 125` each instruction is 1 µs, so the sample happens 1 µs
after the rising edge. The budget is comfortable:

| Contribution | Typical |
|---|---|
| 74HC74 `t_co` (clock to Q out) | ~40 ns |
| PIO input synchroniser (2 stages) | ~24 ns |
| Comparator output rise time | ~100 ns |
| **Total** | **~165 ns** |
| **Available** | **1000 ns** |

Six times the margin. See `docs/HARDWARE.md` for the full analysis.

### Playback (output)

```
mixer ─▶ playback ring ─▶ DMA ─▶ PIO SM1 ─▶ GP2 ─▶ reconstruction filter ─▶ amplifier
```

`out pins, 1` with autopull at 32 bits means the DMA hands over a whole
word at a time and the PIO shifts it out one bit per cycle.

### The DSP (core 1)

Core 1 wakes every 64 µs, renders one word and pushes it into the playback
ring. Three voice types feed the mixer:

| Voice | Source | Status |
|---|---|---|
| `V_OSC` | the 1-bit sine table, with FM and wave shaping | complete |
| `V_SAMP` | the capture ring, resampled | complete, but see below |
| `V_WG` | a Karplus-Strong delay line in the bit domain | implemented, untuned |

---

## The mixer in detail

The mixer is the piece that makes the 1-bit approach work across multiple
voices, so it is worth describing precisely.

### Slot ownership

Each output word has 32 slots. Each voice owns a **disjoint** set of them.
Disjointness is what makes the mix a true sum: two voices never fight over
the same bit, so there is no ordering artefact and no clipping.

### Level as slot count

A voice's level is the number of slots it claims, out of the 32 available:

```
slots(v) = round(level(v) × 32 / 65535)
```

The allocation is *absolute* — a voice at 50 % wants half of the word
whatever else is playing. When the voices ask for more slots than exist,
the request is scaled down so the masks stay disjoint.

The slots nobody owns are filled with the **idle pattern**, which is the
floor of the signal. So the idle pattern is not a silent default but a
real part of the sound: a quiet voice genuinely hands its slots back to it.

### Even distribution

The slots of each voice are spread evenly through the word using a deficit
scheduler, rather than handed out in contiguous blocks. Two reasons:

* the analog front end reconstructs the signal from the *density* of the
  bits, so a voice whose bits are clumped together appears as a
  low-frequency burst rather than as a steady partial;
* the masks rotate by one bit per word, and a blocky allocation would
  rotate as a block, leaving the slot assignment effectively fixed.

### Rotation

Every word, every mask is rotated right by one bit. The *relative*
arrangement between the voices is preserved — they all rotate by the same
amount — but their absolute position in the word drifts.

This is the point. With a static mask, a voice's contribution lands in the
same slot every word, which is a periodic pattern at the word rate and
shows up as a harsh, pitched aliasing artefact. Rotating the mask turns
that into broadband noise, which is far less objectionable and is the
standard trick in 1-bit synthesis.

---

## The resampler in detail

Reading a captured stream at a rate that is not a multiple of the capture
rate is the hardest part of the project.

### Position bookkeeping

The resampler keeps a **16.16 fixed-point** source position. The integer
part advances by a varying amount and the fractional part carries the
remainder, so the *average* rate is exact even when the step is fractional.

### The boxcar

The boxcar tier counts the ones in a window of `L` source bits. The count
is maintained **incrementally**: bits leaving the window are subtracted,
bits entering are added. The cost is proportional to how far the window
moved, not to `L`, so a long window is nearly free at rates close to 1.

### The re-modulator

The boxcar count is a number in `0..L`. Turning it back into a single bit
is where the first-order feedback loop comes in:

```
err += (count - L/2) - (out ? +L/2 : -L/2)
out  = (err >= 0)
```

The reasoning: in the count domain, a full-scale window of `L` ones is `L`,
so the output bit contributes `±L/2` to the comparison — exactly the same
magnitude as the window average does. Over time the loop therefore drives
the *density* of the output bits towards the density of the input window.

That is the whole trick. The output is a genuine 1-bit stream, the level is
preserved, and the only thing lost is the fine structure of the waveform,
which is replaced by shaped noise. The width of the noise band is set by
`L`: longer windows give narrower bands and more latency.

The **REMOD** tier adds fractional interpolation of the window position, so
the decimation phase is not quantised to whole source bits. The difference
between the two window counts is available almost for free:

```
count(pos+1) - count(pos) = bit[pos+L] - bit[pos]
```

so the blend costs two extra bit reads and no extra bookkeeping.

---

## The sine table

The oscillator's waveform is a **2048-bit quarter cycle** — 64 words — that
is mirrored and inverted to make the full 8192-bit cycle.

It is generated by running a first-order delta-sigma modulator over a
quarter sine. A first-order DSM produces a 1-bit stream whose running
average tracks the input, which is exactly what is wanted: the "level" of
the oscillator lives in the density of ones, not in any amplitude field.

### The join check

Mirroring a quarter cycle to make a full one creates three joins. A click
at a join happens when the density steps discontinuously. The generator
measures the density in a 64-bit window on each side of every join and
fails if the difference exceeds 2 % of full scale.

It also checks the harmonic purity of the reconstructed waveform: the
second and third harmonics must sit below 5 % of the fundamental.

### Phase arithmetic

A phase is a 32-bit accumulator whose top 13 bits select the bit position
within the 8192-bit cycle:

```
a = ph >> 19        /* 0 .. 8191 */
```

The frequency of a note is the phase step:

```
step = f_hz × 2³² / fs
```

and two useful properties fall out of the layout:

* adding `0x40000000` advances a **quarter cycle** — quadrature is free;
* adding `0x80000000` advances a **half cycle**, which inverts the
  waveform.

---

## Parameter passing

Core 0 writes parameters; core 1 reads them. The block is published with a
single atomic store of a sequence number after the data is written, so core
1 never sees a half-updated set of parameters:

```c
__atomic_store_n(&pion1_params.seq, generation, __ATOMIC_RELEASE);
```

On the RP2040 an aligned 32-bit store is atomic with respect to the other
core, so this is sufficient without any lock.

---

## Known limitations

These are the things that are not finished yet, stated plainly:

* **The sampler does not track the DMA write position.** It reads the whole
  capture ring rather than the newest audio, so it will play a stale
  buffer. The DMA's current read address gives the position directly and
  the fix is small, but it is not done.
* **The console UI is a placeholder.** A handful of serial keys, not a
  real interface.
* **The waveguide voice is untuned.** It produces plausible output but the
  feedback model is crude.
* **The analog front end has not been built.** All the timing in this
  repository is calculated, not measured.
* **The capture ring's read position is not tracked** on the DSP side, so
  the sampler has no idea how much valid data there is.
