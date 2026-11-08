# EARSHOT: desk acoustic direction finder

- Status: proposed
- Owner: you
- Hardware dir: `hw/earshot/` (not created yet)

## Bottom line

A 100 mm round board: an ESP32-S3 and four PDM MEMS mics on a 60 mm square,
surrounded by a 24-LED ring. It works out where a sound came from and lights
the LED pointing at it, in real time. It also streams bearing, confidence and
raw GCC-PHAT curves over Wi-Fi to a browser polar plot.

The stages build on each other:

| Phase | Target | Difficulty | Why it's fun |
|---|---|---|---|
| 1 | Claps, snaps, keyboard slams | Easy | Proves coherent 4-ch capture + GCC-PHAT end to end |
| 2 | Continuous speech tracking ("who's talking") | Medium | Reverb, VAD, track smoothing: real-world DOA problems |
| 3 | **Operation Bzzz**: localize a mosquito | Probably impossible, try anyway | ~0.5 kHz narrowband source, ~30 dB SPL, 85 mm aperture. Physics is not on your side |

Phase 3 deliberately ends at "LED points at mosquito." The kinetic follow-up
is left to the operator's palm.

Secondary use: point it at the open-plan office and let it silently accuse
whoever is eating chips during a call.

## Why this project

It touches every layer you work in, at hobby scale and cost:

- **Coherent multichannel capture**: sample-synchronous channels or the
  whole thing is a random-bearing generator.
- **DSP on the S3**: FFTs, cross-spectra, sub-sample peak interpolation,
  esp-dsp + PIE SIMD.
- **PCB layout that actually matters**: acoustic port symmetry, PDM clock
  distribution, mic supply isolation from Wi-Fi TX bursts.
- **RF/coexistence**: 2.4 GHz TX current bursts vs. a mic rail sitting 30 mm away.
- **System architecture**: hard real-time audio path on one core,
  network/UI on the other, with a clean seam between them.

## System architecture

```
 4x PDM MEMS ──(CLK shared, 2 data lines, L/R pairs)──► I2S0 PDM RX (HW PDM→PCM)
                                                        │ DMA, 48 kHz, 4ch int16
                                                        ▼
 core 1:  [capture task] ─► frame ring (PSRAM-free, internal SRAM)
          [doa task]     ─► onset/VAD gate → 4x FFT → 6x GCC-PHAT → SRP over
                            azimuth grid → peak + parabolic interp → tracker
                                                        │ bearing, conf, ts
                                                        ▼
 core 0:  [led task]   RMT → 74AHCT1G125 → 24x WS2812B-2020 ring
          [net task]   Wi-Fi STA + esp_http_server WebSocket → browser polar plot
          [cal/dump]   PSRAM 10 s raw 4-ch capture → HTTP .wav download
```

### Key parameters

| Parameter | Value | Note |
|---|---|---|
| fs | 48 kHz | Ts = 20.8 µs ≈ 7.15 mm acoustic path per sample |
| Array | Square, side 60 mm, diagonal 84.9 mm | Fits inside a 24-LED ring on a 100 mm round PCB |
| Max TDOA | 247 µs (diagonal) ≈ 11.9 samples | GCC search window ±13 samples |
| Raw angular res. | ~4.8° at broadside (1-sample step on diagonal) | Parabolic interpolation brings this to ~1° in theory; expect 2–5° RMS in a real room |
| Spatial alias limit | ~2.0 kHz (diagonal), ~2.9 kHz (side), for d < λ/2 | Matters for narrowband phase methods; broadband GCC-PHAT is tolerant |
| Frame | 1024 samples (21.3 ms), 50% overlap | ~94 frames/s |
| Coverage | Azimuth only, 360° | Planar array: can't tell above-plane from below. Fine on a desk |

### Compute budget (estimate, verify at G1)

Per frame: 2 complex FFTs (pack 2 real channels per complex FFT), 6 cross
spectra + PHAT normalize, 6 inverse FFTs. Roughly 8× 1024-pt complex FFT
per frame, ~750 FFT/s. esp-dsp's S3-optimized fc32 FFT should land well under
1 ms each at 240 MHz, so the DOA path should fit in a fraction of core 1.
**Treat that as a guess until it's benchmarked**: measure cycles with
`esp_cpu_get_cycle_count()` around the real pipeline at G1.

## Hardware

### Mic capture topology: the decision that matters

**Use 4 PDM mics on I2S0 PDM RX: one shared PDM CLK, 2 data lines, each
line carrying an L/R pair (SELECT pin high/low).**

Why: one clock gives one sample instant for all four channels, so
inter-channel skew is fixed by geometry and can be calibrated once. With two
separate I2S peripherals or two stereo I2S codecs you get two clock domains,
and any drift between them turns directly into bearing error that changes
with temperature.

- ESP32-S3 I2S0 supports multi-line PDM RX with hardware PDM→PCM conversion.
  **Gate G1 verifies this on your IDF version** (`i2s_pdm_rx_gpio_config_t.dins[]`,
  `SOC_I2S_PDM_MAX_RX_LINES`). If 4-channel PCM conversion doesn't work as
  documented, the fallback is TDM mics (e.g. TDK ICS-52000, daisy-chained,
  one data line). Check lifecycle/stock before committing to it.
- Candidate mics: Infineon IM69D130 (69 dB SNR, 130 dB AOP, bottom port) or
  Knowles SPH0641LU4H-1. Pick on stock, SNR and **phase matching**.
  Sensitivity mismatch is harmless under PHAT; phase mismatch below ~2 kHz
  directly biases the bearing.

### Rest of the BOM (first pass)

| Block | Part (example) | Note |
|---|---|---|
| MCU | ESP32-S3-WROOM-1-N16R8 | PSRAM for raw-capture dump; 16 MB flash for OTA A/B + web UI |
| LEDs | 24× WS2812B-2020 (or SK6812) | 5 V, ring at r ≈ 46 mm |
| LED level shift | 74AHCT1G125 | WS2812B V_IH = 0.7·VDD = 3.5 V at 5 V; a 3.3 V GPIO "usually works" until it doesn't |
| Digital 3V3 | ≥600 mA LDO or buck | Wi-Fi TX peaks are a few hundred mA |
| Mic rail | Separate low-noise LDO + ferrite from 5 V | Isolates mics from TX burst droop |
| Board ID | 2× 1% resistors → ADC pin | Per ADR 0001 |
| USB | USB-C, native S3 USB (D+/D−) | 5.1k CC pull-downs; ESD array on D± |

### Layout rules (write these on the fab drawing, not just here)

1. **Acoustic symmetry beats everything.** All four mics: same footprint,
   same port-hole diameter and depth, same gasket and enclosure cavity.
   Mismatched front volumes give mismatched Helmholtz resonances, which
   means per-channel phase error, which means bearing error. Mount mics on the
   bottom side, port holes through the PCB to a clean top face.
2. **PDM CLK (~3 MHz):** one source, 22–33 Ω series termination at the S3
   pin, short star or tightly controlled daisy. Keep CLK→data skew small
   and equal per line. Not a signal-integrity problem at this length, but
   don't route it next to the antenna feed either.
3. **Antenna keep-out:** WROOM antenna at the board edge, with no copper or
   LEDs under or near it. The LED ring has to break or detour around it, so
   plan the ring with a gap (23 LEDs + a gap at the antenna is fine; the
   firmware maps bearing to the physical LED angle anyway).
4. **Mic supply:** route the mic LDO output as a local island, decoupled at
   every mic. Keep it away from the 3V3 return path that carries TX current.
5. **Mechanical:** soft-mount the board (foam/grommets). Desk thumps
   coupling through the case read as perfectly coherent "sources" directly
   below the array.

## Firmware plan

| Component | Path | Scope |
|---|---|---|
| `board` | `fw/components/board/` | Pin map per board rev, board-ID ADC decode |
| `mic_array` | `fw/components/mic_array/` | I2S PDM RX config, DMA, deinterleave, per-channel delay trim |
| `doa` | `fw/components/doa/` | Onset/VAD gate, GCC-PHAT, SRP grid, interp, α-β tracker. **Pure C, no IDF deps**, so it builds and unit-tests on the host |
| `ring` | `fw/components/ring/` | led_strip/RMT, bearing→LED mapping with decay |
| `net` | `fw/components/net/` | Wi-Fi provisioning, WebSocket stream, `/dump.wav` endpoint |

The host-testable `doa` component is the big lever: feed it synthetic
delayed signals and recorded `/dump.wav` captures from the real board, and
regression-test bearing accuracy in CI without hardware.

## Gates

| Gate | Exit criterion |
|---|---|
| **G0** Devkit bring-up | S3 DevKitC + 4 PDM mic breakouts on a 3D-printed 60 mm jig; 4 channels streaming, no DMA overruns for 1 h |
| **G1** Coherence | Impulse at broadside to a mic pair: measured TDOA = 0 ± 0.2 samples on all 6 pairs. FFT pipeline cycle count measured |
| **G2** Phase 1 | Clap bearing error < 5° RMS at 1–3 m, 36 test angles, normal office |
| **G3** Board rev A | Custom PCB meets G2 numbers with Wi-Fi streaming active (coexistence check) |
| **G4** Phase 2 | Speech tracking holds a single talker with < 10° jitter, switches talkers in < 500 ms |
| **G5** Operation Bzzz | Any repeatable bearing on a real mosquito at > 20 cm. Success here earns a lab notebook entry and smug silence |

## Failure modes and mitigations

| Failure | Symptom | Mitigation |
|---|---|---|
| Inter-channel skew | Constant bearing bias, worse on some pairs | Shared PDM clock; per-channel delay trim stored in NVS from G1 calibration |
| Desk reflection | Biased or doubled peaks, elevation leaking into azimuth | PHAT weighting; gate on the first 2–5 ms after onset (precedence effect); raise the array on a stalk |
| Room reverb | Bearings jump around for sustained sounds | Onset gating for phase 1; coherence-weighted SRP + tracker for phase 2 |
| Wi-Fi TX bursts on mic rail | Periodic ~10 Hz junk (beacon interval) or bursts during streaming | Separate mic LDO + ferrite; verify at G3 with streaming on vs. off |
| Mechanical coupling | Phantom sources "below" on desk taps | Soft mount; reject sources whose energy appears on all channels with near-zero TDOA |
| Spatial aliasing (phase 3) | Multiple equally strong bearings | Use the 2nd–3rd wingbeat harmonics (~0.8–1.5 kHz): better resolution than the fundamental, still under the alias limit |
| Mosquito SNR | Nothing at all | Expected. Narrowband harmonic-comb detector with long integration; accept a ~0.3–0.5 m range in a quiet room (estimate, unproven) |

## Operation Bzzz notes

- Female *Aedes aegypti* wingbeat fundamental is roughly 400–500 Hz (the
  ones that bite). Males run higher. *Culex* is typically lower. Target the
  female band, since the males are harmless and the females are the problem.
- At 500 Hz, λ ≈ 0.69 m against an 85 mm aperture, so beamwidth is huge.
  Resolution has to come from SNR and integration time, not aperture. That's
  why the harmonics matter.
- Mosquito SPL is low and falls 6 dB per doubling of distance. The mic
  self-noise floor (~25 dBA for a 69 dB SNR part) sets the range. This is
  where the project most likely dies, and it's still worth the attempt.

## Open questions

- Enclosure: 3D print vs. an off-the-shelf round puck? This drives the
  port-hole and gasket design (layout rule 1).
- Do we also want an elevation estimate? That needs a 5th mic off-plane
  (tetrahedral geometry) and moves the project from "desk toy" to
  "small acoustic camera".
