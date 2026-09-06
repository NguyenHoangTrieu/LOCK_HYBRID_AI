# Camera_AI_Test1

Real-time face-detection camera on **FRDM-MCXN947**. Captures frames from an
OV7670 camera via SmartDMA, runs a single-class ("face") Edge Impulse FOMO
model per frame, and shows the result as a text status line on a TFT panel.
On detection, saves a snapshot (with a box drawn around the face) to the
TFT panel's onboard microSD card, rate-limited to 1 photo/sec.

**Status:** camera + AI + SD snapshot confirmed working on real hardware
(single-core build). The display moved from an 8-bit-parallel shield to a
2.4" SPI TFT module sharing one bus with its microSD slot and XPT2046
touch controller — confirmed working, currently at **7fps** (target
~24fps, next step paused pending further work; see Known Limitations).
Bus-sharing and touch are build-verified only, not yet tested on hardware.
The dual-core RTOS build (see below) is further along — now showing a
live image on real hardware too.

## Overview

Pipeline: `Camera (OV7670/SmartDMA)` → `AI inference (CPU or Neutron NPU)` → `LCD status text`.

Inference runs on either the CPU (CMSIS-NN) or the Neutron NPU (default,
~4ms/inference on real hardware).

- [ARCHITECTURE.md](ARCHITECTURE.md) — design decisions, memory-conflict
  pitfalls, NPU integration details for this specific build.
- [KNOWLEDGE.md](KNOWLEDGE.md) — plain-language explainer of the underlying
  concepts (DVP camera protocol, coprocessors, NPU/quantization, embedded
  RAM/power domains, SWD debug, DCDC voltage conflicts) for anyone new to
  this domain.
- [WORKLOG.md](WORKLOG.md) — full dated bring-up history.
- Original request: [requirement.md](requirement.md).

## Hardware

| Component | Model / Part | Notes |
|---|---|---|
| Board | FRDM-MCXN947 | |
| Camera | OV7670 | → **J9** (SmartDMA/Camera header) |
| Display | 2.4" TFT, 240x320, SPI (ILI9341-family) | → Arduino header (hardware LPSPI1, shared bus) |
| Touch | XPT2046 (on the same TFT module) | → Arduino header, same shared SPI bus; wired up but not read from anywhere in the app - see Known Limitations |
| microSD | TFT panel's onboard slot (SPI mode, FAT-formatted card) | → Arduino D10..D13, same shared SPI bus (hardware LPSPI1) |
| Debug probe | On-board MCU-Link (CMSIS-DAP) | flashing + serial console |

**Board rework required:** change `SJ16`, `SJ26`, `SJ27` from the right side
to the left side before attaching the camera to J9 (per NXP's reference
design).

### Pinout — Camera (OV7670 → J9)

| Camera signal | MCU pin | J9 pin # |
|---|---|---|
| SIOC / SIOD (SCCB) | P3_2 / P3_3 (LP_FLEXCOMM7) | 19 / 20 |
| XCLK | P2_2 (CLKOUT) | 16 |
| PCLK | P0_5 | — |
| HREF | P0_11 | — |
| VSYNC | P0_4 | — |
| D0..D7 | P1_4, P1_5, P1_6, P1_7, P3_4, P3_5, P1_10, P1_11 | 7,8,9,10,11,12,13,14 |
| 3V3 / GND | — | 21 / 22 |

### Pinout — TFT + microSD + touch (Arduino header, ONE shared hardware SPI bus)

2.4" SPI TFT module (ILI9341-family LCD + onboard microSD slot + XPT2046
touch controller). Not a stacking Arduino shield like the previous panel —
wire each pin to the FRDM-MCXN947's Arduino header with jumper wires.

**All three devices share one physical SPI bus** (SCK/MOSI/MISO on Arduino
D13/D11/D12 — the board's only Arduino-header SPI peripheral, hardware
LPSPI1), each with its own chip-select. This is standard wiring for this
class of module (the LCD's SDI/SCK/SDO, the microSD slot's SD_MOSI/SD_SCK/
SD_MISO, and touch's T_DIN/T_CLK/T_DO are 3 silkscreen labels for the same
3 electrical signals) and keeps pin usage low — only 10 Arduino pins used
total, `D0`-`D7` entirely free. See `source/spi1_bus.c` for how the sharing
works (bus init, per-transaction baud-rate reclaiming, chip-select
handling) — **this bus-sharing logic is new and untested on real
hardware**, more so than a single-device SPI driver would be; see Known
Limitations before assuming it's solid.

| Panel pin | Arduino pin | MCU pin | Notes |
|---|---|---|---|
| SCK / SD_SCK / T_CLK | D13 | P0_25 | shared bus, hardware LPSPI1 SCK |
| SDI(MOSI) / SD_MOSI / T_DIN | D11 | P0_24 | shared bus, hardware LPSPI1 SDO (MCU→devices) |
| SDO(MISO) / SD_MISO / T_DO | D12 | P0_26 | shared bus, hardware LPSPI1 SDI (devices→MCU); needs the internal pull-up `pin_mux.c` already enables here |
| LCD CS | A3 | P0_22 | manual GPIO, LCD-only |
| LCD DC | A2 (legacy single-core build) / **D3** (dual-core build) | P0_14 / **P1_23** | manual GPIO, command/data select - moved to GPIO1 for the dual-core build while this pin's real blocker (core1 has no SAU, needs an explicit `PCNS` Non-Secure grant from core0 - see WORKLOG.md's EIGHTH FOLLOW-UP) was still being tracked down; the GPIO1 move itself probably wasn't the fix, granting `PCNS` was, but it's confirmed working as-is so left unchanged |
| LCD RESET | A4 | P0_15 | manual GPIO |
| LCD LED (backlight) | A5 | P0_23 | manual GPIO, driven high in `LCD_Init()` |
| SD_CS | D10 | P0_27 | **real hardware** LPSPI1 PCS0 - SD-only, see below |
| T_CS | D9 | P0_10 | manual GPIO, touch-only |
| T_IRQ | D8 | P0_28 | manual GPIO input (pull-up enabled), active low when touched |
| VCC, GND | board power header | direct | 3.3V or 5V per the panel's regulator — check the specific module |

Only the microSD slot uses the peripheral's real hardware chip-select
(PCS0/D10) — `SDSPI_Init()` needs to flip its active polarity at runtime,
which only works through actual PCS hardware. The LCD and touch each use a
plain GPIO pin for CS instead, toggled manually around every transfer.

**MADCTL (`0x36`) in `lcd_spi_hw.c`'s `LCD_InitPanel()`**: `MV=1`
(rotation) confirmed correct on real hardware. `BGR` was wrong for this
panel (carried over from the old one) — caused a blue/cyan color cast,
fixed by clearing it (`0x28` → `0x20`). If colors or orientation still
look off, adjust `MV`/`MX`/`MY`/`BGR` there.

**Touch (XPT2046) is wired up but unused** — no touch UI in this
pipeline. `touch_xpt2046.c` provides raw, uncalibrated read functions for
a future feature; X/Y channel mapping is a common-convention guess,
unverified on this panel.

## Getting Started

### Prerequisites

- `arm-none-eabi-gcc`, `west` (via the venv at `../../tools/westenv`)
- Local `mcuxsdk` checkout at `../mcuxsdk`
- `pyocd` for flashing over the on-board MCU-Link

### Build / Flash / Run

```bash
./firmware/camera_ai_demo/build.sh          # build, then flash
./firmware/camera_ai_demo/build.sh build    # build only
./firmware/camera_ai_demo/build.sh flash    # flash the last build
./firmware/camera_ai_demo/build.sh monitor  # serial console (115200-8-N-1)
```

`build.sh` runs `west build -b frdmmcxn947 firmware/camera_ai_demo
--toolchain armgcc -Dcore_id=cm33_core0` from the `../mcuxsdk` west
workspace, then flashes the `.elf` via pyOCD, auto-selecting the probe by
its MCU-Link unique ID. See `../../touch_rgb/README.md` for the
probe-permissions/udev-rule note if `pyocd` needs `sudo`.

**If flashing fails with `DebugPortStart`/`ResetCatchClear`/`ResetSystem`
`WAIT ACK`/`FAULT ACK` errors** — a standing probe/pyOCD/CMSIS-Pack quirk on
this board, unrelated to firmware correctness — see
[ARCHITECTURE.md §5](ARCHITECTURE.md#5-debugging--tooling-notes) for the
working recipe (`nxpdebugmbox` + specific pyOCD flags).

### Dual-core RTOS build (see WORKLOG.md)

A separate, opt-in build (`-DDUALCORE_RTOS=ON`, default `OFF`) boots core1
and splits the app across both cores: core1 runs camera capture + LCD push
+ SD snapshot (`source/main_core1.c`), core0 runs AI inference only
(`source/main_core0.c`), both under FreeRTOS, talking over MCMGR
mailbox-event doorbells (`source/shared/ipc_events.*`) and a shared frame
buffer (`source/shared/ipc_layout.h`) — no RPMsg-Lite. Full migration
history/stage-by-stage bring-up: [ARCHITECTURE.md](ARCHITECTURE.md),
[WORKLOG.md](WORKLOG.md).

This is a **completely separate build directory and command set** from the
single-core build above — the two never share build artifacts and flashing
one never disturbs the other:

```bash
./firmware/camera_ai_demo/build.sh dualcore-build   # build core1, then core0 (core0 embeds core1's image)
./firmware/camera_ai_demo/build.sh dualcore-flash   # flash core0's combined .elf - one flash op, core1 rides along
./firmware/camera_ai_demo/build.sh dualcore-all     # both of the above
./firmware/camera_ai_demo/build.sh monitor          # same serial console works for either build
```

Notes specific to this build:

- **core1 must always build before core0** — core0's link step `.incbin`s
  core1's raw `.bin` (see `board_port/cm33_core0/app.h`), which needs
  core1's build directory to already exist. `dualcore-build` always does
  both in the right order and also force-deletes core0's cached
  `fsl_incbin.S` object file first, since Ninja has no way to know that
  `.incbin` depends on core1's binary — without that, core0 can silently
  re-link a **stale** embedded core1 image after only core1's source
  changed. Always use `dualcore-build` (not a manual `west build` for a
  single core) when iterating, especially on core1 code.
- No `rebuild`/`clean` equivalent for `build_dualcore/` yet — remove it by
  hand (`rm -rf firmware/camera_ai_demo/build_dualcore`) for a truly clean
  dual-core rebuild.
- **The `LCD_CAMERA_PREVIEW`, `AI_MODEL_USE_NPU`, `LCD_ARDUINO_HEADER_BITBANG`,
  and `USB_STREAM_DIAGNOSTIC_DISABLE` flags documented above do NOT apply
  to this build** — `CMakeLists.txt`'s `DUALCORE_RTOS` branch `return()`s
  before any of those `option()`s are even declared. Core1's
  `CameraLcdTask` always runs one fixed loop (camera preview + AI overlay
  + rate-limited snapshot together); there is currently no dual-core
  equivalent of the AI-off, preview-only diagnostic build.
- **Status: Stage 5, CONFIRMED SHOWING A LIVE IMAGE on real hardware
  (2026-09-06)** — camera, LCD push, the core0 AI round-trip, SD
  snapshot, and the on-screen image are all confirmed working. Still
  open: LCD tearing under the cross-core IPC interrupt hasn't been
  re-checked now that the image is visible, and SD write reliability
  degrades after the first few snapshots in a session. See WORKLOG.md's
  latest entries before assuming full stability.
- **Adding a new GPIO pin for core1 to drive? Read this first.** core1
  has no SAU, so it's permanently Non-Secure — GPIO blocks Non-Secure pin
  access per-pin by default (`PCNS` register). An ungranted pin doesn't
  error, it just silently does nothing. Grant it from **core0** before
  `MCMGR_StartCore()` — see `main_core0.c`'s `GPIO0->PCNS`/`GPIO1->PCNS`
  lines, WORKLOG.md's EIGHTH FOLLOW-UP entry, and KNOWLEDGE.md §9.

### Expected output on success

```
Camera_AI_Test1 - FRDM-MCXN947
Camera: OV7670 on J9 SmartDMA/Camera header
Display: Arduino-header LCD status text (camera + AI hook)

Camera: OV7670 detected on J9 (PID=0x76 VER=0x73 confirmed), 320x240 @ 30 fps.
AI_MODEL_Init: Neutron NPU face detector ready (72x72 input, 1 class(es), arena used 94388/122880 bytes)
LCD: hardware SPI (LPSPI1, shared bus) on the Arduino header
AI_MODEL_RunInference: total classifier time = 3957us (3ms)
AI result: box[0] label=face x=<N> y=<N> w=<N> h=<N> score=<N>%
```

LCD shows two text lines — `FACE: 1`/`FACE: 0`, and `CAPTURE: 1`/`CAPTURE: 0`
(lit for 4s right after a snapshot is saved, see "Snapshot on Face
Detection" below) — not a live image.

## Configuration

Pass any of these to `build`/`rebuild`/`all` as `-D<FLAG>=ON|OFF`.

| Flag | Default | Effect |
|---|---|---|
| `AI_MODEL_USE_NPU` | `ON` | Inference backend: Neutron NPU (`ON`, ~4ms/inference) vs. CPU + CMSIS-NN via Edge Impulse SDK (`OFF`). Same detection logic either way. |
| `LCD_CAMERA_PREVIEW` | `OFF` | Skip AI, stream raw camera feed to the LCD as fast as frames arrive — for focusing the lens by eye. Prints `LCD preview: N fps` once/sec over UART — see Known Limitations for the fps math/target. |
| `LCD_ARDUINO_HEADER_BITBANG` | `ON` | `ON`: drive TFT via hardware SPI on the Arduino header, shared with the microSD/touch bus (default). `OFF`: J8 header (FlexIO/bit-bang) — abandoned, no touch/SD sharing there, see [ARCHITECTURE.md §4](ARCHITECTURE.md#4-known-constraints--trade-offs). |
| `USB_STREAM_DIAGNOSTIC_DISABLE` | `ON` | `OFF` opts into the abandoned UVC webcam streaming path (doesn't drive the LCD) — see [ARCHITECTURE.md §4](ARCHITECTURE.md#4-known-constraints--trade-offs). |

`AI_MODEL_USE_NPU` and `LCD_CAMERA_PREVIEW` are independent of the LCD/USB
flags; combining `LCD_CAMERA_PREVIEW=ON` with
`USB_STREAM_DIAGNOSTIC_DISABLE=OFF` builds but isn't a meaningful
combination.

### Example commands

```bash
# Default build (Neutron NPU, camera+AI+LCD status text, no USB)
./firmware/camera_ai_demo/build.sh build

# Lens-focus diagnostic: raw camera feed straight to the LCD, no AI
./firmware/camera_ai_demo/build.sh build -DLCD_CAMERA_PREVIEW=ON

# CPU + CMSIS-NN inference instead of the Neutron NPU
./firmware/camera_ai_demo/build.sh build -DAI_MODEL_USE_NPU=OFF

# J8 header (FlexIO) instead of the Arduino header's SPI panel - abandoned, see ARCHITECTURE.md §4
./firmware/camera_ai_demo/build.sh build -DLCD_ARDUINO_HEADER_BITBANG=OFF

# Flags combine freely - build only (no flash), CPU backend + lens-focus preview
./firmware/camera_ai_demo/build.sh build -DAI_MODEL_USE_NPU=OFF -DLCD_CAMERA_PREVIEW=ON
```

`build` builds without flashing; `all` (or bare `./build.sh`) builds then
flashes; `rebuild` cleans `build/` first - needed when switching flags
that change which files compile, since flags otherwise stick in the CMake
cache.

## AI Model

Edge Impulse project **"Face_Detection_NXP"** (ID `1095726`, deploy v2),
FOMO detector, 72x72 int8 input, single class `face`, trained on
WIDER FACE + DarkFace (**both CC-BY-NC-ND/research-only — not cleared for
commercial use as-is**).

| | Neutron NPU (default) | CPU + CMSIS-NN |
|---|---|---|
| Build flag | (default) | `-DAI_MODEL_USE_NPU=OFF` |
| Source | `source/ai/model_runner_npu.cpp` | `source/ai/model_runner.cpp` |
| Tensor arena | 120KB static (`m_data`), 94,388 bytes used | 112,460 bytes (`m_sramx` primary + `m_data` overflow) |
| Measured on hardware | ~3.96ms/inference | not yet separately measured for this model |

To swap in a different/retrained model: re-export from Edge Impulse Studio
(new C++ library for the CPU path; new `.tflite` run through
`neutron_converter` for the NPU path) and update
`model_runner.cpp`/`model_runner_npu.cpp` if tensor shapes or class labels
changed. See [ARCHITECTURE.md §2](ARCHITECTURE.md#2-components) for how
the NPU conversion and integration actually work.

## Snapshot on Face Detection

On face detection, `snapshot.c` draws a green box (via `bbox_overlay.c`)
into the frame buffer and saves it as an uncompressed 16-bit BMP
(`FACE0001.BMP`, ...) to the SD card, rate-limited to 1 capture/sec.
Filenames never overwrite a previous session's snapshots.

The box is the model's raw grid-cell box (small relative to a real face,
since FOMO doesn't regress an actual box size) - a cosmetic expansion was
tried and reverted on purpose. See
[ARCHITECTURE.md §2](ARCHITECTURE.md#2-components).

The LCD shows a `CAPTURE: 1` status line for 4s after each save (the box
itself is only in the saved file, never drawn on the LCD). No SD card →
logged once at boot, then snapshot capture silently no-ops every frame
after.

Every save logs how long the actual SD card write took (DWT cycle
counter, same technique `AI_MODEL_RunInference` uses):
```
Snapshot: saved FACE0030.BMP (write took 187342us, 187ms)
```
Originally ~3.3s/save - the SD bus was stuck at the 400kHz identification
speed instead of switching up. Fixed (`SD_SPI_OPERATING_BAUDRATE`, now
8MHz); see [WORKLOG.md](WORKLOG.md).

Writes the frame buffer directly to the file with one `f_write()` call -
no second full-frame buffer needed, which matters since `m_data` is
already >90% used. See [ARCHITECTURE.md §2](ARCHITECTURE.md#2-components).

## Project Structure

```
Camera_AI_Test1/
  README.md, ARCHITECTURE.md, WORKLOG.md, requirement.md
  firmware/camera_ai_demo/
    CMakeLists.txt, prj.conf, build.sh
    board_port/            <- pin muxing, hardware_init.c, ei_sramx.ld (board.c/clock_config.c come from the SDK)
    source/
      main.c                <- capture -> AI inference -> LCD status-text loop
      fault_handler.c        <- HardFault register-decode dump handler
      spi1_bus.c/h            <- shared hardware LPSPI1 bus (LCD + microSD + touch), see file header comment
      camera/                <- OV7670 + SmartDMA driver (from NXP reference)
      display/                <- lcd_spi_hw.c (default, Arduino header, hardware SPI), touch_xpt2046.c (Arduino header only), lcd_bitbang.c/lcd_flexio_mculcd.c (abandoned J8 path), text_overlay.c
      ai/
        model_runner.h        <- shared inference API, both backends implement
        model_runner.cpp      <- CPU + CMSIS-NN backend
        model_runner_npu.cpp  <- Neutron NPU backend (default)
        ei_sramx_alloc.c/h    <- custom allocator for the CPU path's tensor arena in m_sramx
        edge_impulse/          <- Edge Impulse C++ library export
        neutron/                <- NPU-converted model (tflite_learn_..._npu.tflite/.h)
      usb/                    <- abandoned UVC streaming path
      storage/
        snapshot.c/h           <- rate-limited box-draw + BMP save on face detection
        sd_spi_disk.c/h         <- shared-bus SD-over-SPI + FatFs diskio glue (see ARCHITECTURE.md §2)
        ffconf.h                 <- hand-written FatFs config (RAM-minimal: FF_FS_TINY=1, no LFN)
```

## Known Limitations

- LCD shows text status lines only, not a live image or bounding boxes
  (by design, to save RAM — see "Abandoned Features" below); the box drawn
  on face detection only ever exists in the saved BMP file.
- CPU-path inference timing for the current model hasn't been separately
  measured on hardware yet (NPU path is measured: ~4ms/inference).
- Not yet validated against a real face specifically — pipeline (capture →
  resize → inference → decode) confirmed working end-to-end with real
  non-flat camera data, but detection accuracy is only backed by Edge
  Impulse Studio's validation/test-set metrics so far (see WORKLOG.md).
- The Arduino-header panel is a 2.4" SPI TFT module sharing one bus with
  its onboard microSD slot and XPT2046 touch controller. **LCD path
  confirmed on real hardware** (correct orientation, no corruption; one
  bug found and fixed - `BGR` was wrong, causing a blue/cyan cast).
  Bus-sharing baud-reclaim logic and touch are still build-verified only,
  not tested on hardware - suspect them first if SD snapshots ever come
  back corrupted (`sd_spi_disk.c`, `lcd_spi_hw.c`).
- **fps target (~24fps) not reached - confirmed at 7fps**, up from 2fps
  via two real-hardware bug fixes (missing `kLPSPI_MasterPcsContinuous`
  flag; `SPI1_BUS_SetBaudRate()` not recomputing per-transfer delay
  registers) - see WORKLOG.md for the full trail. **eDMA was attempted
  and abandoned** - hung on real hardware (RX eDMA channel never signals
  done), root cause not found; reverted to the working CPU-polled path.
  If the image looks glitchy/torn at 24MHz, lower `LCD_SPI_BAUDRATE_HZ`
  toward 2-6MHz first.
- SD card mount/init **confirmed working on real hardware**, after fixing
  5 separate bugs (boot hang, a floating MISO line needing an internal
  pull-up, a false timeout on every write, missing font glyphs) - see
  [WORKLOG.md](WORKLOG.md) /
  [ARCHITECTURE.md §5](ARCHITECTURE.md#5-debugging--tooling-notes). Not
  yet separately confirmed: an actual face-triggered capture succeeding
  end-to-end.

## Abandoned Features

Not part of the current default build; kept in the tree (still compile in
most cases) for reference. Full trail in [WORKLOG.md](WORKLOG.md).

| Feature | Why abandoned | Still buildable via |
|---|---|---|
| Live camera image + bounding-box overlay on LCD | Too much data over the bit-bang bus per frame; reverted to 1-line text status | `source/display/bbox_overlay.c/h` (no longer unused — reused by the SD snapshot feature above, just not drawn to the LCD) |
| J8 FlexIO LCD header (hardware-accelerated bus) | Wiring/init confirmed good, but pixel data came out as noise — unresolved bus-speed/signal-integrity issue | `-DLCD_ARDUINO_HEADER_BITBANG=OFF` |
| USB Video Class (UVC) webcam streaming | Genuine hardware conflict, not fixable in software — see [ARCHITECTURE.md §4](ARCHITECTURE.md#4-known-constraints--trade-offs) | `-DUSB_STREAM_DIAGNOSTIC_DISABLE=OFF` (won't drive the LCD) |

## History

Started on the Arduino-header TFT design, briefly tried J8 FlexIO and USB
Video Class as alternatives (both abandoned - see table above), then
camera+LCD bring-up, AI integration (see
[ARCHITECTURE.md §3](ARCHITECTURE.md#3-key-design-decisions)), and the
Neutron NPU backend as a speed upgrade. The model itself started as a
3-class drowsy-eye detector, later swapped for the current single-class
`face` detector. Full dated trail: [WORKLOG.md](WORKLOG.md).
</content>
