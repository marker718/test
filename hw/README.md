# hw/: board designs (KiCad 9)

```
hw/
├── lib/                  shared, project-agnostic libraries
│   ├── symbols/          *.kicad_sym
│   ├── footprints/       <lib>.pretty/*.kicad_mod
│   └── 3dmodels/         *.step
└── <board>/              one KiCad project per board, e.g. hw/earshot/
    ├── <board>.kicad_pro
    ├── <board>.kicad_sch
    ├── <board>.kicad_pcb
    ├── sym-lib-table     points at ${KIPRJMOD}/../lib/...
    ├── fp-lib-table
    └── README.md         rev history, bring-up notes, errata
```

## Rules

- **Library paths are relative.** Use `${KIPRJMOD}/../lib/...` in the project
  lib tables, never absolute paths or a personal global library. If it isn't
  in this repo, CI can't see it and neither can your next laptop.
- **Fab outputs are not committed.** Gerbers, drill files, BOM and CPL are
  produced by `kicad-cli` from a tagged commit (`hw-<board>-revX`). The tag is
  the release, so the outputs can always be regenerated.
- **Every rev gets a board ID strap.** A resistor divider on one ADC pin,
  different value per rev, so firmware knows what it's running on. See
  `docs/adr/0001-monorepo-layout.md`.
- **ERC/DRC must be clean in CI** (`.github/workflows/hw.yml`). If you waive a
  violation, do it with a rule exclusion in the project, not by ignoring CI.
- **Silkscreen carries the rev and the short git hash** (use a text variable,
  e.g. `${GIT_REV}`, filled in at fab-output time) so the board on the bench
  can be matched to the commit it came from.
