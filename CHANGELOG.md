# Changelog

All notable changes to the `hako-powerboard` driver, tracked in
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) form.

## [0.2.0] — 2026-04-30

First packaged release. v0.1 was an internal milestone (read-only fan
plus PWM read/write) that landed in the repository but was never tagged.

### Added — hwmon class device

- `power[1-4]_input` (microwatts) with per-rev calibration:
  - HW 2.0 — linear, slope = 3.574, intercept = -1.375
  - HW 2.1 — linear, slope = 3.284, intercept = -1.069
  - HW 2.4+ — linear, slope = 925.25, intercept = 0
  - HW 2.2 / 2.3 — 4 × 11 multivariate regression matching
    HakoFoundry's `_calculate_wattage_22` exactly (verified against
    Foundry's float reference on 10 test vectors)
- `power[1-4]_label` — `Shunt 1` … `Shunt 4`
- `update_interval` — RW chip attr exposing the per-class cache TTL
  (milliseconds, range 100–60000, default 1000)
- `pwm[1-3]_enable` is now writable (accepts `1` for manual mode; rejects
  other values with `-EOPNOTSUPP`)

### Added — interface-device attrs (under `/sys/bus/usb/devices/<bus>-<port>:1.1/`)

- `default_pwm[1-3]` (RW, 0–255) — EEPROM-stored boot-time PWM, equivalent
  to HakoFoundry's "Default Fan Speed" settings page. Read maps to `P:`,
  write maps to `F:`. Writing also restores live PWM via a follow-up `U:`
  so the running fans aren't disturbed.

### Added — operational

- DKMS package (`PACKAGE_VERSION="0.2.0"`)
- udev rule (`udev/99-hako-powerboard.rules`) to rebind the device's CDC
  Data interface from `cdc_acm` to `hako_powerboard` whenever cdc_acm
  binds it. Treat installation as a one-way migration step — incompatible
  with HakoFoundry running concurrently.
- Top-level `Makefile` with `install` / `uninstall` / `dkms-install` /
  `install-udev` targets. `make install` is a one-shot deploy that pulls
  the udev rule into `/etc/udev/rules.d/` and registers + builds the module
  via DKMS against the working tree (symlinked into `/usr/src/`).
- Safety push on disconnect: before deasserting DTR, the driver issues
  `U:` with the EEPROM defaults so fans land in the user-configured safe
  state on `rmmod` / DKMS reinstall. Uses `usb_driver.soft_unbind = 1` to
  keep the interface alive across teardown.
- Rate-limited `dev_warn_ratelimited` when `pwm[N]` is written while the
  jumper says motherboard owns PWM (writes are silently ignored at the
  hardware level in that mode).

### Added — driver internals

- USB driver matching VID/PID `2341:8036` filtered to interface class
  `USB_CLASS_CDC_DATA`
- Probe-time discrimination: send `V:`, parse `<hw>,<fw>,<location>`,
  return `-ENODEV` on parse failure so `cdc_acm` gets the device back if
  the underlying device is a real Arduino Leonardo
- DTR/RTS assertion via `SET_CONTROL_LINE_STATE` on the CDC Comm interface
  before the first query (Arduino's CDC firmware ignores serial input
  without DTR)
- Per-device DMA-safe TX scratch buffer (`devm_kzalloc`); `hako_send_cmd`
  copies into it before `usb_bulk_msg`
- Per-class jiffies cache for fan / power readings; PWM cache becomes
  authoritative after the first `P:` read (because `P:` returns the
  EEPROM-stored value, not the live `U:`-set value)
- Quirks bitfield encoding: `FW22_U_INVERT`, `FW23_P_INVERT`,
  `J_INVERT_PRE25` (canonicalizes `jumper_mode` to "1 = firmware-active"
  regardless of hw rev)

### Notes

- `pwm[1-3]_label` is intentionally absent: `HWMON_PWM_LABEL` is not in
  Linux 6.19's `linux/hwmon.h` enum. Add it when targeting a kernel where
  the slot was added.
- Cooling-device class binding is not implemented; users wire their own
  fan-curve daemon (fancontrol, thermald) to `pwm[1-3]`.
