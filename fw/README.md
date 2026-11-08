# fw/: ESP32-S3 firmware

ESP-IDF project. Pinned toolchain: **ESP-IDF v5.4.2** (same version in CI).

```
fw/
├── CMakeLists.txt        project; derives PROJECT_VER from fw-v* tags
├── sdkconfig.defaults    committed config (sdkconfig itself is generated)
├── main/                 app entry point
└── components/           project-local components (drivers, DSP, board support)
```

## Build / flash

```sh
cd fw
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # native USB-Serial-JTAG
idf.py -p /dev/ttyUSB0 flash monitor     # DevKitC UART bridge
```

No local IDF:

```sh
docker run --rm -v "$PWD/..":/project -w /project/fw espressif/idf:v5.4.2 idf.py build
```

## Rules

- **`sdkconfig.defaults` is the source of truth.** `sdkconfig` is gitignored.
  After `menuconfig`, run `idf.py save-defconfig`, review the diff, commit.
- **`dependencies.lock` is committed** once managed components show up, so
  builds stay reproducible.
- **Version** comes from `git describe --match "fw-v*"` at configure time.
  After tagging, run `idf.py reconfigure` or the binary keeps the old string.
- **Board-specific code** goes in `components/board_<name>/`, selected by the
  board-rev ADC strap at runtime rather than by `#ifdef` forests.
