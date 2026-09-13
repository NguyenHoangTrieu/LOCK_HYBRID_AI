# LOCK_EDGE_BASEBOARD Library

Project-local KiCad library for the Edge Node HAT board — an Arduino-header
shield meant to plug onto the FRDM-MCXN947's Arduino connector, and to stay
mechanically compatible with the Arduino Uno V3 connector on an STM32N6
Nucleo board.

Loaded automatically via the project's `sym-lib-table` / `fp-lib-table`
(all paths relative to `${KIPRJMOD}`, i.e. this project's own folder — no
system-wide library install needed).

| Part | Symbol lib | Footprint | Source | License | 3D model |
|---|---|---|---|---|---|
| 2.4in 240x320 SPI TFT + touch, 14-pin | `tft-240x320-spi` | `TFT_240x320_SPI_Touch_14Pin` | Copied from the `Test_FOX_M4` project (`FOX_M4_FAST_DESIGN`) | project-internal | none found |
| ESP32-C5-DevKit V2.0 (generic, not Espressif's official DevKitC-1) | `esp32-c5-devkit-v2` | `ESP32-C5-DevKit-V2` | Self-drawn from a product photo (see note below) | n/a (original) | none |
| Arduino header (power/analog/digital rows, no ICSP) | `arduino-header` | `Arduino_Header` | Pad layout from [Alarm-Siren/arduino-kicad-library](https://github.com/Alarm-Siren/arduino-kicad-library)'s `Arduino_Uno_R3_Shield`, trimmed to just the connector (see note below) | CC-BY-SA 4.0 + electronic-design exception | none |
| RFID-RC522 (MFRC522), SPI, 8-pin | `rfid-rc522` | `RFID-RC522_Header_1x08_P2.54mm` | Self-drawn from the standard RC522 pinout (SDA/SCK/MOSI/MISO/IRQ/GND/RST/3.3V) — no modern, freely-downloadable-without-an-account KiCad library found | n/a (original) | placeholder: standard KiCad 1x8 pin-header STEP model |
| Servo motor, 3-pin (SIG/VCC/GND) | `servo-motor-3pin` | `Servo_Header_1x03_P2.54mm` | Self-drawn — generic 2.54mm 3-pin header, not something any manufacturer publishes as a named "servo" part | n/a (original) | placeholder: standard KiCad 1x3 pin-header STEP model |

## Notes / things to verify before fab

- **RC522**: pin order matches the standard RC522 breakout pinout; the
  dashed reference outline on the footprint's `Cmts.User` layer is an
  *approximate* module size, not measured from a datasheet — check against
  your actual board.
- **Servo header**: pin order (SIG/VCC/GND) is the common convention, but
  some servo brands swap GND and Signal at the connector — check against
  your actual servo before mating.
- **Arduino header**: this is *not* the original `Arduino_Uno_R3_Shield` —
  the board-outline artwork (USB/barrel-jack silkscreen, mounting holes)
  and the 2x3 ICSP block were stripped out, keeping only the 3 single-row
  headers (Power, Analog, Digital, 31 signals + 1 unconnected mechanical
  pin) actually needed to plug this HAT onto the FRDM-MCXN947's Arduino
  header. Pad *positions* — including the non-uniform D7/D8 gap — are
  unchanged from the source library, so it should stay mechanically
  compatible with an STM32N6 Nucleo's Arduino connector too. If you need
  ICSP, it'll have to be added back separately.
- **ESP32-C5-DevKit V2.0 — READ BEFORE FAB.** This is a generic board
  (dual USB-C, RGB LED on GPIO27, BOOT/RST buttons), confirmed *not* to be
  Espressif's official ESP32-C5-DevKitC-1 — the GPIO set visible in the
  product photo doesn't match Espressif's published J1/J3 pinout. Drawn
  entirely from a compressed product photo with no zoom/measurement tool
  available, so:
  - **Pin pitch (2.54mm) and count (16+16=32) are confident.**
  - **Column-to-column spacing (25.4mm) and board outline are placeholder
    guesses, not measured** — the dashed `Cmts.User` outline is only a
    rough reference.
  - **8 of the 32 pin names could not be read reliably** and are left as
    `UNVERIFIED_R5`...`UNVERIFIED_R10`, `UNVERIFIED_R14`, `UNVERIFIED_R15`
    in the symbol (right-hand header, rows 5-10 and 14-15) — the full
    per-pin confidence breakdown is in the symbol's `Description` property.
  - Do not route anything to this footprint/symbol until pin spacing is
    measured on the real board and the unverified pins are confirmed.
- **TFT**: no 3D STEP model available from any source found without
  creating an account (SnapEDA/GrabCAD gate it behind sign-in). If you
  download one yourself, hand it over and it can be wired into the
  footprint's `(model ...)` block.
