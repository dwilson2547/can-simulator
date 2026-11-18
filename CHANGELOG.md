# Changelog — can_simulator

## 2026-11-17

### Fixed
- **A bus-off left the sim silent until it was rebooted.** `twai_initiate_recovery()`
  ends in `STOPPED`, not `RUNNING`, and nothing called `twai_start()` again, so every
  later transmit failed. It is now restarted once recovery finishes (`[CAN] recovered`).
- **The console was never served during a dense replay.** It only ran in gaps over 5 ms,
  so `status` and `stop` went unanswered for as long as the capture had none. It now
  also runs every 20 ms during playback.

### Added
- A `[stats]` line every 2 s during playback: frames queued, transmits refused (queue
  full or driver not running), TWAI state and error counters. This is what showed the
  stopped driver above: 1 frame queued, the rest refused, `state=STOPPED`.
- `txtest` console command: stops playback and the TWAI driver, drives TXD high then
  low by hand, and counts how often RXD reads low. A working transceiver echoes TXD
  (0/1000 then 1000/1000). It needs no meter on the bench.

## 2026-11-04

### Added
- **Acks-required mode, to reproduce an isolated OBD port.** A 2024 Blazer recorded
  through the listen-only dongle came back with each frame repeated back-to-back 4-10
  times per millisecond (83% of frames copies, most seconds missing an ID). The likely
  cause (⚠ unverified until a bench run reproduces it): that car's OBD port sits on its
  own segment, so nothing acks and every frame is resent. The sim's `NO_ACK` mode hides
  exactly this, so it can now require acks like a real ECU:
  - `ack on` / `ack off` on the console (restarts the TWAI driver; shown by `status`);
  - env `xiao_s3_ack` (`-D SIM_REQUIRE_ACK=1`) boots in that mode, for tests powered from
    the dongle's port with no console.
  The default build (`xiao_s3`) is unchanged: `NO_ACK`.

## 2026-06-11 (session 2)

### Fixed
- **Timestamp unit bug** — SavvyCAN CSV timestamps are raw `micros()` values (microseconds), not seconds. The timing formula was multiplying by 1e6, turning a 2ms inter-frame gap into a 35-minute wait. Result was always one frame transmitted then silence. Fix: removed `* 1e6` from `targetUs` calculation.
- **TWAI mode changed from NORMAL to NO_ACK** — the phone companion dongle runs in `TWAI_MODE_LISTEN_ONLY` and never sends ACK bits. In `NORMAL` mode the simulator's TX error counter climbed to 128 (error-passive) because every transmission went unacknowledged. Changed to `TWAI_MODE_NO_ACK` so the simulator doesn't require acknowledgement from any listener — correct for a bench replay device.
- **TX queue depth increased from 5 to 32** — the default `TWAI_GENERAL_CONFIG_DEFAULT` queue depth of 5 was too shallow for burst replay of dense log files, causing silent frame drops.
- **Added bus-off recovery** — `twai_initiate_recovery()` is called periodically during playback if TWAI enters bus-off state. Previously the controller would stay dead until the simulator was power-cycled.
- **Added TWAI state to `status` command** — now prints state, tx_err, rx_err, tx_failed, and rx_missed counters alongside playback state.
- **Gap clamping — bidirectional** — inter-frame gaps are now clamped to `[0, maxGap]`. Large forward jumps (pauses, corrupt timestamps) are capped at the max gap (default 1 s). Negative gaps caused by timestamp rollback after a corrupt frame are clamped to zero so replay doesn't blast frames or stall. A `[gap]` line is printed on serial whenever capping occurs.
- **Added `maxgap <ms>` console command** — sets the maximum inter-frame gap in milliseconds (default 1000). Useful when a capture contains intentional pauses you want to preserve or compress further.

### Notes on SavvyCAN CSV format
- Timestamps are raw `micros()` values in microseconds — large integers, not seconds. A capture starting 11 seconds into device uptime will have timestamps like `11209025`.
- CAN IDs are plain hex without `0x` prefix (e.g. `0000034A`). `strtoul` with base 16 handles this correctly.
- All rows are padded to 8 data fields regardless of actual DLC. The parser reads only `LEN` bytes and ignores the rest.
- Corrupt frames with bogus timestamps (e.g. a single-byte frame with ID 0x00000000 and timestamp 4 billion) can appear in real captures due to GVRET framing noise. The gap clamp handles these transparently.

## 2026-06-11

### Added
- Initial project created under `robo-services/can_simulator/`
- `firmware/xiao_can_simulator/platformio.ini` — PlatformIO project targeting Seeed Studio XIAO ESP32S3, using pioarduino platform-espressif32 55.03.34 (same as ESP32RET sniffer for toolchain consistency)
- `firmware/xiao_can_simulator/src/main.cpp` — complete CAN bus replay firmware:
  - Reads SavvyCAN CSV log files from a SPI SD card (FSPI bus on D8/D9/D10, CS on D3)
  - Parses SavvyCAN format (`Time Stamp,ID,Extended,Dir,Bus,LEN,D1..D8`) including both Rx and Tx rows so the full bus traffic is reproduced
  - Drives TWAI (CAN) in NORMAL mode at 500 kbps via TJA1051T transceiver on D0/D1
  - Accurate timing replay using `esp_timer_get_time()` with a hybrid busy-wait/yield strategy: yields every 1 ms during long inter-frame gaps so the serial console stays responsive, then busy-waits for the final ≤5 ms for precision
  - Auto-plays the first `.csv` file found on the SD at boot
  - Loop mode on by default; restarts from the top of the file when playback finishes
  - Non-blocking serial console at 115200 baud with commands: `ls`, `play <file>`, `stop`/`q`, `loop`, `speed <n>`, `status`, `help`
  - Speed multiplier (`speed 0.5` / `speed 2`) scales all inter-frame delays

### Why
The team needed a bench CAN simulator so vehicle testing doesn't require going to the car for every firmware iteration. Log files captured from the 2008 Chevy Impala via the ESP32RET sniffer are replayed on a second XIAO S3 + TJA1051T to reproduce real bus traffic on the workbench.
