# doa: azimuth estimator (GCC-PHAT + SRP)

Pure C11 + libm, no ESP-IDF dependencies. The same sources build as an IDF
component on the S3 and as a static library on the host for testing.

```
include/doa.h        public API
src/doa.c            estimator
src/doa_fft.[ch]     private radix-2 FFT (portable; esp-dsp swap point)
test_host/           synthetic scene generator + test harness
```

## Pipeline

1. Hann window, then FFT with **two real channels per complex FFT** (4 mics = 2 FFTs).
2. Optional per-channel delay trim (calibration), applied as a phase rotation.
3. GCC-PHAT on all 6 pairs, band-limited to 200 Hz–8 kHz, with **two pairs per
   complex IFFT** (3 IFFTs). Five 1024-pt complex transforms per frame in total.
4. SRP: sum of pair GCCs at the expected TDOA (Catmull-Rom interpolation between
   lags). Three stages:
   - coarse: 5° azimuth steps × in-plane scale rings
   - fine: pick the scale at full azimuth resolution
   - final: full 1° azimuth scan at that scale
5. Parabolic peak refinement. Confidence = mean pair GCC at the peak.

### Why the scale rings (elevation tolerance)

A planar array sees the arrival vector projected onto its plane, so every
TDOA shrinks by cos(el). The PHAT main lobe is only ~±3 samples wide with an
8 kHz band edge, while the largest TDOA is ~12 samples. An in-plane-only search
therefore misses sources above the desk outright: at 60° elevation it gave
**105° RMS error**. Searching rings of constant cos(el) (spaced ≤ 0.2, down to
`el_max_deg`, default 70°) fixes it at about the same SRP cost, since the
coarse pass is stride-5. Elevation is searched over but never reported. A
planar array can't tell up from down, and its cos(el) resolution near the
plane is poor.

## Host tests

```sh
cmake -S fw/components/doa/test_host -B build/doa-host -G Ninja
cmake --build build/doa-host && ctest --test-dir build/doa-host --output-on-failure
build/doa-host/doa_test -v     # per-angle detail
```

ASan + UBSan are on by default (`-DDOA_SANITIZE=OFF` to disable). CI runs this
on every `fw/**` change.

The scene generator (`test_host/synth.c`) is independent of the code under
test: 3D geometry, near-field 1/r, a double-precision 96-tap Blackman-sinc
fractional delay, coherent image sources for reflections, per-channel hardware
skew, and independent sensor noise. It shares no FFT and no TDOA table with
the estimator.

### Current results (default config, 72 azimuths per sweep)

| Scenario | RMS err | Max err |
|---|---|---|
| Far-field white noise, SNR 20 dB | 0.18° | 0.45° |
| Near-field 0.25 m | 0.22° | 0.56° |
| Clap (5 ms decay), SNR 20 dB | 0.14° | 0.33° |
| SNR 0 dB / −5 dB | 1.26° / 2.52° | 3.83° / 6.61° |
| Wall reflection +90°, −3 dB, +0.83 ms | 0.29° | 0.75° |
| Desk mirror, el ±20°, gain 0.8 | 0.23° | 0.72° |
| Elevation 45° / 60° / 70° | 0.21° / 0.38° / 0.57° | 0.49° / 1.06° / 1.30° |
| 1.5-sample skew on ch1: raw → calibrated | 5.09° → 0.16° | 7.47° → 0.46° |
| Confidence: coherent / diffuse / silence | 0.976 / ≤ 0.086 / 0 | |

These are best-case numbers: free field, white source, ideal mics. The real
room (G2) will be worse. That's what `/dump.wav` captures from hardware are for.

## Known limits / next steps

- **Sound from almost directly overhead** (above `el_max_deg`). At 85° the
  result is 3.1° RMS / 8.1° max on synthetic data, and degrades further toward
  vertical, where azimuth is undefined. Confidence stays ≥ 0.72 because the
  frame is still coherent, so **confidence does not flag this case**. If it
  bites in practice, expose the winning in-plane scale and gate on it.
- **FFT backend**: swap in esp-dsp `dsps_fft2r_fc32` behind `doa_fft.h`, and
  keep the portable version as the host reference.
- **No onset gate or tracker yet.** Phase 1 needs an energy-ratio onset
  detector feeding `doa_process()`; phase 2 needs an α-β tracker on top.
