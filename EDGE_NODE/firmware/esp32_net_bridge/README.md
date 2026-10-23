# esp32_net_bridge

ESP-IDF v6.0 firmware for the ESP32-C5-DevKit on `LOCK_EDGE_BASEBOARD`
(U3). Bridges the MCXN947's SPI link to the Cloud Backend over WiFi -
CON-2 and CI-1..5 in `SYSTEM_REQUIREMENTS.md`. **Builds clean (`idf.py
build`, confirmed) and compiles against the matching MCXN947-side driver
(`../camera_ai_demo/source/network/network_client_esp32.c`, also
confirmed building) - neither has been tested on real hardware talking to
each other.** No two physical boards were available to verify the actual
wire-level link.

## Why this exists, and the one real hardware constraint that shapes it

The schematic (`PCB_DES/LOCK_EDGE_BASEBOARD.kicad_sch`, read directly via
the KiCad MCP tools, not guessed) wires exactly 4 nets to U3:
`ESP32_CS`/`SPI_SCK`/`SPI_MOSI`/`SPI_MISO` - no handshake/ready GPIO. The
MCXN947 is the SPI master (it already owns this bus for the LCD/SD/RFID).
ESP-IDF's `spi_slave` driver normally wants a handshake line so the
master never clocks before the slave has a receive buffer queued; without
one, `main/protocol.h` works around it with fixed-size full-duplex
frames, slow master-side polling, and sequence numbers - see that file's
header comment for the full reasoning. If this turns out too fragile on
real hardware, the right fix is almost certainly a handshake GPIO on a
future board revision, not more protocol cleverness.

Pin mapping (GPIO6/8/9/10) comes from the schematic's own symbol pin
names for U3, not a guess - see `main/spi_bridge.c`'s header comment.

## Building

```bash
source ~/esp/esp-idf/export.sh    # or wherever IDF_PATH points
cd EDGE_NODE/firmware/esp32_net_bridge
idf.py set-target esp32c5
idf.py menuconfig                 # set WiFi SSID/password, backend URL, device API key - see Kconfig.projbuild
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.defaults` already sets the 2MB "single app, large" partition
table - the default 1MB one is too small once WiFi+mbedTLS+HTTP client
are linked in (confirmed: the default build overflows it).

## What it does

1. Connects to WiFi (station mode, `wifi_sta.c`).
2. Serves as an SPI slave (`spi_bridge.c`) implementing `protocol.h`'s
   frame types: card attendance, face attendance (chunked upload, not
   exercised yet - the MCXN947 side has no JPEG encoder wired up), config
   fetch, heartbeat.
3. Each request becomes one HTTP call to the Cloud Backend
   (`backend_client.c`, hits the exact routes in `CLOUD_SERVER/README.md`)
   with the device's `X-Device-Key`, run on a separate worker task so the
   SPI transceive loop never blocks on the network.
4. The heartbeat response carries the server's clock, which the MCXN947
   side uses to set its own time (CI-5) - there's no RTC or SNTP client
   on the device otherwise.

## Known gaps

- **Untested wire-level link** - see above.
- **TLS** - `CONFIG_LOCK_HYBRID_BACKEND_SKIP_TLS_VERIFY` exists for
  pointing at a dev server with a self-signed cert; leave it off for
  anything real. CLOUD_SERVER itself doesn't terminate TLS either (see
  its README) - put a reverse proxy in front for CI-1 to actually mean
  something.
- **Face upload path** - implemented, compiles, nothing to test it
  against yet (device side has no real JPEG to send).
- **WiFi credentials in plaintext sdkconfig** - fine for a dev build,
  revisit (NVS provisioning / flash encryption) before shipping a device.
