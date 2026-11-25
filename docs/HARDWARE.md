# PION/1 — hardware

The analog front end is not optional. A 1-bit signal has no amplitude
information, so the analog circuit *is* the amplitude stage: it decides what
"level" means, what the noise floor is, and what the frequency response is.

Everything here is calculated from datasheets, not measured on a bench. The
numbers are called out so they can be checked against real parts.

---

## Pin assignment

| Pico pin | Direction | Function |
|---|---|---|
| GP0 | in | capture input — the flip-flop's Q output |
| GP1 | out | flip-flop clock, driven by PIO side-set |
| GP2 | out | playback output — the 1-bit signal |
| GP3 | out (optional) | modulator clock, for the experiments below |
| GP4/GP5 | UART | console fallback (stdio) |

GP1 and GP3 are driven by the PIO, so they must not be used as ordinary
GPIO. `pio_gpio_init()` takes care of the function select.

The console is normally the **USB CDC**, not the UART. GP4/GP5 are a
fallback for a UART adapter, and are set explicitly in `CMakeLists.txt`
because the Pico's board header puts UART0 on GP0/GP1 by default, which
would collide with the capture path.

---

## The console

The firmware talks to the host over USB CDC. On the host it appears as
`/dev/serial/by-id/usb-Raspberry_Pi_PION-1-if00` — the name is built from
the USB product string and the chip's unique board ID, so it is stable
across reboots and replugs and does not change when other USB devices
are plugged in.

```sh
python3 tools/pion1_console.py     # attach and print
minicom -D /dev/serial/by-id/usb-Raspberry_Pi_PION-1-if00 -b 115200
```

The firmware prints its banner whenever the host asserts DTR, so a host
that attaches late, or re-attaches after a disconnect, gets the greeting
as though it had been there all along.

### Flashing

```sh
tools/pion1_flash.sh
```

This builds, waits for the Pico to appear in BOOTSEL mode, copies the
UF2, and waits for the CDC to come back. If the Pico is running a build
without USB support it will be invisible to the host, and BOOTSEL mode is
the only way in: hold the **BOOTSEL** button while pressing **RESET**
(or while plugging the cable in).

The firmware exposes the RP-series reset interface, so a running PION/1
can be dropped into the bootloader over USB without touching the
buttons:

```sh
picotool reboot -u -f
```

That is what `--reboot` does in `tools/pion1_flash.sh`.

---

## The input stage

The job of the input stage is to turn an analog signal into a clean 1-bit
stream at 500 kHz, and to close a feedback loop around it.

```
                      +-----------+
   input ──▶ [preamp]──▶ comparator ├──▶ D ──▶ Q ──▶ GP0
                 ▲        (LM311)       │   (74HC74)
                 │                      │
                 │      GP1 (side-set) ─┴──▶ CLK
                 │
                 └──── feedback resistor
```

### Why a flip-flop?

The feedback loop is the whole point of the design, and a comparator on its
own cannot be latched into a stable state. The D flip-flop sits between
the comparator and the feedback node:

* the comparator decides the *next* bit, continuously;
* the flip-flop holds the *current* bit, stable for a whole bit period;
* the PIO samples the held value.

Without the flip-flop the loop would be combinational and would either
oscillate or latch up. With it, the loop is sampled and stable by
construction — which is what makes the whole thing predictable.

The flip-flop also means the feedback path has a **one bit period** of
latency (2 µs). The analog feedback network has to be designed around
that; see the loop stability note below.

### Comparator

An LM311 is the usual choice: it is open-collector, so the output needs a
pull-up, and it has a wide input range including the ground reference that
makes single-supply operation awkward. The pull-up also gives it a slow
rise time, which is useful here — it limits the edge rate into the
flip-flop.

An LM393 works too, on a single supply, but its input common-mode range
does not include ground, so the input has to be biased above the rail.

### The D flip-flop

A 74HC74 is fine at these speeds. The only timing requirement is `t_co`,
the delay from clock edge to output valid:

| Part | `t_co` (typ) | `t_co` (max) |
|---|---|---|
| 74HC74 | 40 ns | 60 ns |
| 74HCT74 | 25 ns | 40 ns |

A 74HCT74 gives more margin, and the higher noise margin of the HCT
family is helpful when the flip-flop is sitting next to an analog stage.

**Never substitute a 74LS74 here.** The LS family is much slower and much
noisier, and `t_co` can exceed 100 ns.

### The feedback resistor

The feedback path decides how much of the output is fed back into the
summing node, which sets the loop gain and therefore how the circuit
behaves as an oscillator.

A higher feedback ratio makes the circuit oscillate more readily and gives
a stronger fundamental, but it also pushes the loop closer to instability
and lets the noise floor rise. Start around 0.3–0.5 and adjust by ear.

The loop's **one bit period of latency** (2 µs) has to be much shorter
than the period of the frequency being generated. For a 1 kHz note the
period is 1 ms, so 2 µs is 0.2 % — negligible. But at 20 kHz the period is
50 µs and 2 µs is 4 %, which starts to matter. The loop is therefore
usable up to roughly the point where the bit period becomes a significant
fraction of the note period.

---

## The output stage

The 1-bit output has to be turned back into an analog signal. This is a
reconstruction filter: the fundamental plus controlled noise.

```
   GP2 ──▶ [RC low-pass] ──▶ [buffer/amp] ──▶ output
            fc ≈ 5–15 kHz
```

The reconstruction filter's cutoff sets how much of the noise the 1-bit
process generates survives. The 1-bit noise is broadband, so:

* too low a cutoff and the treble is gone, but the noise is too;
* too high and the raw quantisation noise is audible.

For most tones, a single-pole RC at 5–15 kHz is enough. A second-order
filter (two RC stages, or one RC into an op-amp) is better if the noise is
too prominent.

The reconstruction filter is the numerical equivalent of the boxcar
average in the host renderer, which is why the rendered WAV sounds like the
hardware is supposed to.

---

## Timing budget

The capture path's timing has to fit inside one bit period (2 µs).

| Contribution | Typical | Worst case |
|---|---|---|
| 74HC74 `t_co` | 40 ns | 60 ns |
| PIO input synchroniser (2 stages @ 125 MHz) | 24 ns | 32 ns |
| Comparator (LM311 with pull-up) | 100 ns | 300 ns |
| PCB and connector | 5 ns | 20 ns |
| **Total** | **~170 ns** | **~410 ns** |
| **Available (1 µs from edge to sample)** | **1000 ns** | **1000 ns** |
| **Margin** | **~5.9×** | **~2.4×** |

The margin is comfortable, which is the point of using the two-instruction
capture program: the sample happens a full microsecond after the clock
edge, so the circuit does not have to be fast, only *predictable*.

### A note on the modulator clock SM

`pion1_modclock` generates a free-running square wave for experiments
where the modulator needs a symmetric drive rather than the capture SM's
one-shot-per-bit clock. It is not used by the default path.

---

## The clock

The PIO timing assumes a 125 MHz system clock. That is the RP2040's
default, but it is worth being explicit about it:

* `set_sys_clock_khz(125000u, true)` is called at startup, and the `true`
  means *fail rather than fall back* if the clock cannot be set.
* The PIO dividers are integer, so the bit rate is exact.

If you change the system clock, the bit rate changes with it: it is
`clk_sys / clkdiv`, and the project assumes `125 MHz / 250 = 500 kHz`.

---

## Bill of materials

| Qty | Part | Purpose | Notes |
|---|---|---|---|
| 1 | Raspberry Pi Pico | the synth | RP2040, plain Pico is fine |
| 1 | LM311 | comparator | open-collector, needs a pull-up |
| 1 | 74HCT74 | D flip-flop | one half used; the other half is spare |
| 1 | 74HC14 | hex inverter | useful for clock conditioning |
| 2 | 100 nF ceramic | decoupling | close to the ICs |
| 1 | 10 kΩ | comparator pull-up | value sets the edge rate |
| 1 | 100 kΩ | feedback resistor | start value, tune it |
| 2 | 10 kΩ + 100 nF | reconstruction filter | RC low-pass |
| 1 | TL072 or similar | output buffer | dual op-amp |
| — | 100 nF ceramic | decoupling | close to the ICs |

---

## Bring-up order

Do it in this order, and check each stage before moving on. The signal
chain is short enough that a single mistake is hard to find later.

1. **Clock.** Flash the firmware and confirm it boots and prints over the
   console (`python3 tools/pion1_console.py`). If there is no output,
   check the USB cable — many are charge-only.

2. **Playback.** Without any input hardware, connect GP2 through a 100 nF
   capacitor to a scope or to the reconstruction filter. You should see a
   440 Hz square wave. If you do not, the problem is in the firmware or
   the PIO, not the analog side.

3. **Output filter.** Build the reconstruction filter and confirm the
   square wave becomes a sine. This is where a wrong RC value shows up
   first.

4. **Comparator.** Feed a low-frequency sine (100 Hz) into the
   comparator input and look at the output on a scope. It should be a clean
   square wave.

5. **Flip-flop.** Clock the flip-flop from a signal generator at 500 kHz
   and check that Q follows D with the expected delay.

6. **Feedback.** Close the loop with the feedback resistor. The circuit
   should start oscillating. This is the moment the instrument becomes
   alive.

7. **Audio in.** With the loop closed, feed a signal into the summing
   node and the output should track it.

---

## Debugging

A logic analyser on GP0 and GP1 is the single most useful tool here, and
any analyser that can trigger on a rising edge will do. Look for:

* **GP1 not toggling** — the PIO is not running, or the side-set pin is
  misconfigured. Check that `pion1_bitstream_start()` was called and that
  the SM is enabled.
* **GP0 static** — the comparator or the flip-flop is not working. This is
  an analog problem, not a firmware one.
* **GP0 toggling but the output is silent** — the playback path. Check the
  playback ring is being filled: `pion1_playback_push()` returns 0 when the
  ring is full.
* **The output is noisy but the pitch is right** — the reconstruction
  filter is too wide. Lower its cutoff.
* **The pitch drifts** — the two PIO state machines are not running in
  sync. They must be enabled with `pio_enable_sm_mask_in_sync()`, never
  with separate `pio_sm_set_enabled()` calls.

---

## Known issues

* The capture ring's write position is not tracked by the DSP, so the
  sampler plays a stale buffer rather than the newest audio.
* There is no input protection on the analog front end. Do not connect
  the Pico's GPIO to anything above 3.3 V.
* The comparator's input range does not include ground on the LM393; the
  LM311 does, but needs a pull-up.
