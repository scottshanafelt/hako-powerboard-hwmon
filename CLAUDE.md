# CLAUDE.md

Guidance for Claude Code when working in this repo.

## What this is

A Linux kernel `hwmon` driver for the HakoForge Hako-Core Powerboard (USB CDC ACM, VID/PID `2341:8036`). v0.2 (current shipped version) exposes 3× PWM (RW), 3× tach (R), 4× shunt wattage (R, with HW-rev-aware linear + HW 2.2/2.3 multivariate calibration), `default_pwm[1-3]` (RW EEPROM defaults), and device metadata (`hardware_revision`, `firmware_version`, `location`, `jumper_mode`, `update_interval`). Packaged as DKMS with a udev auto-rebind rule. Eventual upstream submission is a stretch goal.

**`PLAN.md` is the authoritative design doc.** Read it before starting any non-trivial task — it covers protocol facts, USB binding strategy, sysfs attribute map, file layout, implementation order, and verification.

## License — non-negotiable

GPL-2.0-or-later. Per-file SPDX `// SPDX-License-Identifier: GPL-2.0-or-later`. C source uses `MODULE_LICENSE("GPL v2")` (kernel's recognized string for GPL-2.0-compatible). Do **not** switch to GPL-3.0 — kernel modules must be GPL-2.0-compatible to resolve `EXPORT_SYMBOL_GPL` symbols (every hwmon/USB API the driver needs). The `LICENSE` file holds canonical GPLv2 text on purpose.

## Build & test environment

Kernel modules don't build on macOS. Build and test on a Linux box with the powerboard physically attached, kernel headers installed under `/lib/modules/$(uname -r)/build`, and `dkms` available.

Typical dev loop with this repo cloned on the test box:

```sh
git pull
sudo make install        # udev rule + DKMS register/build/install
sudo make dkms-install   # rebuild from src/ after source changes
```

For a quick out-of-tree rebuild without DKMS:

```sh
sudo rmmod hako_powerboard
make module
sudo insmod src/hako_powerboard.ko
```

The udev rebind rule triggers on cdc_acm bind events but does not interfere with `insmod`/`rmmod` cycles on a device already bound to the driver.

## Code conventions

- Single C file at `src/hako_powerboard.c`, ~600–800 LOC target
- Modern hwmon API: `hwmon_chip_info` + `hwmon_channel_info`, not legacy `SENSOR_DEVICE_ATTR` macros
- Reference drivers: `drivers/hwmon/aquacomputer_d5next.c` (chip_info layout), `drivers/hwmon/corsair-psu.c` (request/response with jiffies-based staleness cache)
- PWM writes use the `U:` command (volatile), never `F:` (EEPROM) — `fancontrol`-driven per-second writes would wear out EEPROM
- Driver canonicalizes `jumper_mode` sysfs attr to "1 = firmware/powerboard active" regardless of HW revision; HW 2.5 has inverted polarity in the wire protocol (see PLAN.md quirks)

## General gotchas

- `.mcp.json` is gitignored. Do not commit it.
- The board enumerates as a generic Arduino Leonardo with no unique `iSerial`. The driver discriminates from real Leonardos by sending `V:` in `probe()` and returning `-ENODEV` on parse failure so `cdc_acm` gets the device back.

## Driver implementation gotchas (lessons learned)

- **DMA-safe TX scratch buffer.** `usb_bulk_msg` warns ("transfer buffer is on stack" from `usb_hcd_map_urb_for_dma`) and can hang/taint the kernel if you pass a stack-allocated buffer. The PWM-write path builds a string into a per-device scratch buffer allocated via `devm_kzalloc` in probe and memcpy'd into before the bulk transfer. Apply this pattern to any future TX path.
- **`F:` writes EEPROM AND side-effects live PWM** — firmware behavior, not a driver choice. The driver works around it with a snapshot+replay pattern: snapshot live cache, send `F:`, send `U:` with the snapshot to restore live PWM. ~300 ms fan dip during the F:→U: window. FW 2.2 inversion applies only to `U:`, never `F:`.
- **`P:` returns the EEPROM-stored PWM, not the live PWM.** After a `U:` write, the driver's cache becomes authoritative for `pwm[N]` reads — without that, `cat pwm1` post-write returns the stale EEPROM value.
- **`soft_unbind = 1` is required** on the `usb_driver` struct so the disconnect-time safety push (read EEPROM defaults, write them via `U:`) actually completes. Without it, the USB core terminates URBs and disables endpoints before `disconnect()` runs and the final round-trip fails with `-ENOENT`. On true unplug (`USB_STATE_NOTATTACHED`) the core ignores the hint and the safety push fails fast — fine, device is gone anyway.
- **Firmware has no comms-loss watchdog and no shutdown-time fallback.** Empirically tested: the board holds the last `U:` value through DTR drop. EEPROM `P:` defaults only apply on a power cycle. So fan policy must include "what happens on rmmod/crash". The driver's safety push handles graceful unload; kernel-panic / hard-crash leaves fans wherever last set (userspace problem).
- **Rmmod → cdc_acm bind race.** Manually rebinding cdc_acm immediately after `rmmod hako_powerboard` can race — the bind tries before USB disconnect propagates. If you script this kind of toggle, sleep ~1 s between rmmod and the cdc_acm bind.

## Hardware quirks worth remembering

- **Per-row tach, not per-port.** Each fan wall has multiple physical 4-pin headers wired in parallel for PWM/12V/GND, but only the *first* header per row has its tach line wired back to the firmware. A 4-pin fan on header 2/3 will respond to PWM but report 0 RPM. See `Documentation/hako-powerboard.rst` troubleshooting section.
- **Pin↔row swap on the wire.** All HW revs use command parameter order `row2, row3, row1` (pin↔row swap). The driver exposes row order externally; conversion happens internally.
- **HW 2.5 jumper polarity is inverted** vs older revs — `J:=1` = firmware-active on 2.5, `J:=0` = firmware-active on pre-2.5. Driver canonicalizes `jumper_mode` sysfs attr to "1 = firmware-active" regardless of rev. The `HAKO_Q_J_INVERT_PRE25` quirk bit is set on pre-2.5 (action-oriented direction).
- **FW 2.2: `U:` payload is `100 - row`** per channel. **FW 2.3: `P:` response is `255 - x`** per channel. Both handled at parse/format time inside the driver.

## fan2go integration notes (if you choose to use fan2go for fan control)

This driver is a generic hwmon producer; userspace can use `fancontrol`, `fan2go`, custom scripts, etc. The notes below come from a real fan2go deployment and are worth knowing if you go that route — none of this is intrinsic to the driver.

- **Default `controlAlgorithm` is PID and can oscillate** with the PWM↔RPM map fan2go calibrates for typical disk-shelf fans — PWM bounces between target and 100% every ~10 s. Add `controlAlgorithm: { direct: {} }` per fan to write the curve PWM through the PWM map without RPM-based feedback. Don't go back to PID without re-tuning P/I/D gains.
- **`steps:` value units are subtle.** Keys are temperatures (°C); values are strings — bare integers are raw PWM (0-255), `40%` is a percentage. Curves expressed as `35: 40` look reasonable but mean "at 35°C, write PWM 40 (≈16%)" which is below typical `startPwm`, so the fan never starts. Use `%` notation to express Foundry-style 40-100 % curves.
- **First-run calibration takes ~10 min** per fan: fan2go sweeps PWM 0→255 to learn each fan's RPM response on first start, regardless of explicit `startPwm`/`minPwm`. Disks/CPU are without active cooling during the sweep. Cached in `/var/lib/fan2go/fan2go.db`; subsequent restarts are instant.
- **Below first knee, linear curves clamp to the first step's value** (not extrapolated). Above the last step, clamp to the last step's value. So a curve `35: 40%, 45: 80%, 50: 100%` returns 40 % for any temp ≤ 35 °C.
- **`function: maximum` aggregation works** as you'd expect — evaluates each sub-curve and returns the max. Useful for "this fan wall cools several things; drive it from whichever is hottest."
