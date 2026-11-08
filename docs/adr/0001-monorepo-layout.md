# 0001: Single monorepo for firmware, hardware and docs

- Status: accepted
- Date: 2026-11-08

## Context

Firmware and PCB revisions are coupled: pin maps, strap options, peripheral
choices and errata workarounds all depend on the exact board rev. Keeping them
in separate repos means you have to work out "which fw commit matches the rev B
board on my bench" from commit dates and memory.

## Decision

1. One repo: `fw/` (ESP-IDF), `hw/` (KiCad), `docs/`.
2. Independent release streams via prefixed tags:
   - `fw-vMAJOR.MINOR.PATCH`
   - `hw-<board>-rev<X>`
3. Firmware version = `git describe --match "fw-v*"` (see `fw/CMakeLists.txt`).
4. CI jobs are path-filtered: `fw/**` triggers the firmware build, `hw/**`
   triggers ERC/DRC. Neither blocks the other.
5. **Board identity at runtime:** every board rev has a resistor divider on a
   dedicated ADC pin (`BOARD_ID`). Firmware reads it at boot and selects the
   board-support config. Suggested scheme: 1% resistors, 100k total, eight
   bins spaced ~400 mV apart on a 3.3 V rail, decoded with a ±150 mV window.
   That leaves plenty of margin over ADC INL, resistor tolerance and rail error.
   An out-of-window read is a hard fault ("unknown board"), not a silent default.

## Consequences

- Good: one commit is a consistent fw+hw snapshot. Bisecting works across both.
- Good: fw can refuse to run, or switch to a degraded mode, on a board rev it
  doesn't know, instead of driving a pin that's now shorted to a regulator.
- Bad: history is noisier. Mitigate with scoped commit prefixes (`fw:`, `hw:`, `docs:`).
- Bad: KiCad binary-ish churn (3D models) bloats the repo. Keep STEP files in
  `hw/lib/3dmodels/`, deduplicated; consider Git LFS if it passes ~100 MB.
- Costs one ADC pin and two resistors per board for the ID strap. Worth it.
