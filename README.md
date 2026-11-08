# test

Hardware + firmware monorepo. Firmware, board design and docs live together so
that any commit is a consistent snapshot of "this firmware runs on this board".

```
.
├── fw/        ESP-IDF firmware (ESP32-S3)
├── hw/        KiCad projects, one directory per board + shared libs
├── docs/      Architecture notes, ADRs, project specs
└── .github/   CI: fw build and hw ERC/DRC, path-filtered
```

## Quick start

```sh
# Firmware (needs ESP-IDF v5.4.x on PATH)
cd fw
idf.py build
idf.py -p /dev/ttyACM0 flash monitor

# Or without a local IDF install
docker run --rm -v "$PWD":/project -w /project/fw espressif/idf:v5.4.2 idf.py build
```

## Conventions

| Thing | Convention |
|---|---|
| Firmware release tag | `fw-vMAJOR.MINOR.PATCH` (e.g. `fw-v0.3.1`) |
| Hardware release tag | `hw-<board>-rev<X>` (e.g. `hw-earshot-revA`) |
| Firmware version string | `git describe --match "fw-v*"`, baked in at configure time |
| Board identity | Board rev is read at boot from an ADC strap (see `docs/adr/0001`) |
| Generated outputs | Never committed (`build/`, `sdkconfig`, fab outputs). CI produces them |

Why the tag prefixes: in a monorepo a plain `git describe` happily picks up a
hardware tag as the firmware version. The prefix keeps the two streams apart.

See [`docs/adr/0001-monorepo-layout.md`](docs/adr/0001-monorepo-layout.md) for
the reasoning, and [`docs/projects/earshot.md`](docs/projects/earshot.md) for
the first thing to build here.
