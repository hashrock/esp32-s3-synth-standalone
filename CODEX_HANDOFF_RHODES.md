# Codex handoff: Rhodes physical modeling synth on ESP32-S3

## Goal
Port the **audibly successful** Rhodes physical-model prototype (`rhodes-physics-lab.html`) to the existing ESP32-S3 e-piano firmware. Preserve its sound, particularly the realistic attack, and measure CPU / audio deadline impact. Do not replace it with generic FM or an attack-noise sample.

## Environment
- Local repository (user's machine): `~/code/hardware/epiano` (verify actual path and current branch before editing).
- Board: Freenove ESP32-S3, 240 MHz dual-core, 8 MB PSRAM; PCM5102A I2S DAC.
- I2S: BCLK GPIO16, LRCK GPIO17, DOUT GPIO18, MCLK unused; PCM5102A SCK tied to GND.
- Working engine: 48 kHz / 16-bit stereo, DMA 64 frames x 4, USB MIDI host, currently 16 voices. Existing audio scheduling must remain stable.
- Reference HTML is an **offline pre-render** proof of concept, NOT realtime: outputs 4.2 sec buffers at 44.1 kHz and integrates physics at 176.4 kHz.

## Source model (inspect HTML before coding)
- `original()`: baseline modal-only, *not* the target sound.
- `simulate(midi, a, false)`: preferred first target **mode 02**: hammer -> tine -> linear pickup.
- `simulate(midi, a, true)`: mode 03 nonlinear pickup; optional after 02.
- 5 ideal clamped-free cantilever eigenfrequency ratios: `[1, 6.267, 17.548, 34.386, 56.84]` (actual used modes limited by frequency).
- Mode pickup/contact shape weights `[1, -.43, .2, -.1, .05]`.
- Per-mode integration: `a_j = shape_j*F/mass_j - 2*gamma_j*v_j - omega_j^2*q_j`; semi-implicit Euler update `v += a*dt; q += v*dt`.
- Hammer: mass 0.010 kg; `hx=-0.0007`, `hv=.35+2.8*velocity`; spring coefficient `k=2.5e7*exp((stiffness-.5)*2.3)`; damping `eta=.01+3.2*loss`.
- Contact: `compression=hx - sum(shape_j*q_j)`, `relativeVelocity=hv - sum(shape_j*v_j)`; `F = max(0, k*compression^1.5 * max(0,1+eta*relativeVelocity))`, capped at 150 N; hammer `hv += -F/massHammer*dt; hx += hv*dt`.
- Linear pickup: `voltage = sum(shape_j*v_j)/(2*pi*220*0.0004)`; LPF, DC reject, scaling/tanh follow. Nonlinear mode uses `dPhi/dx * velocity` approximation with clipping of the derivative factor.
- Reference uses `FS = SR*4`, `SR = 44100`, `DT=1/FS` and 4.2 sec per note.
- Note **contactMs in prototype counts total contact samples**, which may include multiple impacts; do not treat as continuous first contact duration without checking.

## Implementation tasks
1. Inspect current ESP-IDF firmware, MIDI/note lifecycle, existing synth and task/core layout; write a brief integration plan. Avoid unrelated refactors.
2. Extract the physical simulator into a self-contained C++ voice, with `note_on(note, velocity)`, `render(float* ... frames)`, `note_off()`; no allocations, `pow`, `exp`, or trig in the per-sample hot path if avoidable. Precompute note coefficients; consider `float` with stability tests, while keeping a high-precision reference harness.
3. First port single-voice **mode 02** at 48 kHz, 4x internal oversampling (~192 kHz). Keep exact full integration as a reference; test against JS-rendered WAV or programmatic golden sample, accounting for 44.1 vs 48 kHz differences.
4. Crucial optimization: after last hammer contact and reliable separation, propagate each free mode using an exact damped-oscillator state transition (matrix or complex recurrence) at the 48 kHz output rate. Preserve **q and v** at handover, and the correct velocity for pickup; ensure no discontinuity or change in apparent decay. Do not switch back while hammer may still re-contact; implement/contact-test a safe criterion.
5. CPU measurements on target: single note, 4/8/16 sustained notes, 4/8/16 simultaneous Note Ons, fast retriggers, high/low register, worst-case hard velocity. Record per-audio-block cycles, peak and average CPU, DMA underruns, and missed deadlines. Watch for expensive `powf` in contact processing and temporary CPU spikes.
6. If 4x contact is too expensive, evaluate 2x or adaptively oversampled hammer only; A/B sound comparison and numerical stability required. Also test an event/sparse contact excitation approximation only after baseline exists.
7. Add nonlinear pickup as a **toggle** only if CPU permits; compare modes 02 and 03 at matched loudness. Do not enable it by default because earlier simplified nonlinear pickup sounded overly metallic/distorted.
8. Preserve USB MIDI, voice stealing, note-off damping and DAC routing. Existing 16-note polyphony is a target, not an assertion that the new algorithm can achieve it.
9. Add a small deterministic native/off-target harness that renders WAV test notes and validates finite outputs, no clipping/stuck notes, repeatability, stable decay, and convergence of 2x/4x/8x oversampling. If the original model is unstable at some notes/velocities, document and correct the issue; don't blindly preserve bugs.

## Acceptance criteria
- A/B reference tone for mode 02: realistic attack **and** natural sustain, no distinct metallic bell tail that dominated older modal-only version.
- No audible clicks at contact/free-vibration handover or note-off.
- Target firmware builds; single-voice playback stable at 48 kHz first; polyphony only increased after CPU evidence.
- Benchmark numbers and implementation tradeoffs reported, including measured worst-case block usage and underruns.
- No major regressions to original firmware; new engine selectable alongside original FM synth until validated.

## Delivery
- Source changes in `~/code/hardware/epiano`, documentation of model and CPU measurements, WAV golden/test clips where practical, commit only following the repository's conventions. Clearly state what was not tested on actual hardware.
- Start by reading `rhodes-physics-lab.html`, then existing firmware. Ask for missing source file if it isn't accessible; do not invent its contents.
