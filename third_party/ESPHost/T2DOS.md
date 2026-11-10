# ESPHost for T2DOS

The ESP-Hosted driver from https://github.com/Networking-for-Arduino/ESPHost
(LGPL-2.1, see LICENSE), used to build the kernel for the Challenger 2350
WiFi/BLE with `--library third_party/ESPHost`.

It is upstream `main` at f6e66ab plus PR #16
(https://github.com/Networking-for-Arduino/ESPHost/pull/16): on RP2040 it
busy-waits instead of calling `delayMicroseconds()` inside the handshake GPIO
interrupt. arduino-pico 6.2.0 ships an older copy that hangs the board once
WiFi is up (it also lacks #15). Only `src/` and the metadata are copied; the
upstream examples and extras are left out. `src/ESPHost_t2dos.h` is a T2DOS
marker file.

Drop this copy once arduino-pico's bundled ESPHost has both fixes.
