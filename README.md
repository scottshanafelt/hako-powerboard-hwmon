# hako-powerboard-hwmon

Linux `hwmon` driver for the [HakoForge Hako-Core Powerboard](https://github.com/HakoForge/HakoFoundry) — exposes the board as a standard kernel sensor device so `sensors`, `fancontrol`, `pwmconfig`, Prometheus `node_exporter`, Home Assistant's `systemmonitor` integration, and anything else that speaks `/sys/class/hwmon` work without custom glue.

The Hako-Core Powerboard is a USB CDC ACM device that distributes 12 V to disk backplanes and drives three rows of 4-pin PWM fans, with four ADC shunts for power monitoring. The vendor's userspace daemon (HakoFoundry) talks to the board directly and computes fan curves itself; without this driver, the board doesn't appear in `/sys/class/hwmon` at all and the standard tooling can't see it.

This driver replaces the daemon at the kernel boundary. Fan policy stays in userspace where you choose your own tool — `fancontrol`, `fan2go`, `thermald`, custom scripts.

## Status

**v0.3.0 — feature-complete, DKMS-packaged, tested on real hardware.** Fan tach (R), PWM (RW), 4× shunt wattage (R), EEPROM-default PWM (RW), and device-metadata attributes. v0.3.0 is a reliability and cleanup release — no ABI or feature changes from v0.2.0 (query/response cross-talk fixes, locking cleanup, and added sanity tests); see `CHANGELOG.md`. Builds and run-tested through **Linux 7.0** (DKMS rebuilds cleanly across kernel upgrades with no source changes). Eventual upstream submission to `linux-hwmon` is a longer-term goal; for now distribution is via DKMS.

## Compatibility

- **Kernel**: Linux 5.10 or newer (uses the modern `hwmon_chip_info` API). Built and run-tested through Linux 7.0; DKMS rebuilds cleanly across kernel upgrades with no source changes.
- **Hardware**: Hako-Core Powerboard hardware revisions 2.0, 2.1, 2.2, 2.3, 2.4, 2.5 — per-rev quirks and wattage calibration auto-detected from the board's `V:` query at probe time. Read your revision after install from `/sys/bus/usb/devices/<bus>-<port>:1.1/hardware_revision`, or check via HakoFoundry before switching over.
- **Distros**: any with DKMS — Debian/Ubuntu, Fedora, Arch, openSUSE, etc.

## Prerequisites

- `dkms` and matching kernel headers for your running kernel:
  - Debian / Ubuntu: `sudo apt install dkms linux-headers-$(uname -r)`
  - Fedora: `sudo dnf install dkms kernel-devel`
  - Arch: `sudo pacman -S dkms linux-headers`
- A C toolchain (`make`, `gcc`) — usually pulled in transitively.
- A Hako-Core Powerboard plugged in over USB.

## Install

```sh
git clone https://github.com/scottshanafelt/hako-powerboard-hwmon
cd hako-powerboard-hwmon
sudo make install
```

This does two things:

1. Installs the udev rule (`/etc/udev/rules.d/99-hako-powerboard.rules`) that rebinds the powerboard's CDC Data interface from `cdc_acm` to `hako_powerboard` whenever it appears.
2. Registers the module with DKMS, builds it against your running kernel, and installs it. DKMS will rebuild automatically on future kernel upgrades.

After install, the driver claims the device at boot and on hotplug.

> **Installing the udev rule is a one-way migration.** It pulls the data interface away from `cdc_acm`, so `/dev/ttyACM0` for the powerboard disappears and HakoFoundry can no longer open it. Install only after you've decided to manage the board from the kernel driver. If you want the module without taking the device away from `cdc_acm`, run `sudo make dkms-install` (DKMS only, no udev rule).

## Verify

```sh
sensors
```

You should see something like:

```
hako_powerboard-virtual-0
Adapter: Virtual device
Row 1:       1320 RPM
Row 2:          0 RPM
Row 3:          0 RPM
Shunt 1:       0.00 W
Shunt 2:      12.97 mW
Shunt 3:      14.69 W
Shunt 4:       0.00 W
pwm1:             64%  (mode = pwm)  MANUAL CONTROL
pwm2:              0%  (mode = pwm)  MANUAL CONTROL
pwm3:             64%  (mode = pwm)  MANUAL CONTROL
```

If `sensors` doesn't list `hako_powerboard`, see the [Troubleshooting section in the full reference](Documentation/hako-powerboard.rst#troubleshooting).

## Use

Find your hwmon path:

```sh
HWMON=$(for d in /sys/class/hwmon/*; do
  [ "$(cat "$d/name" 2>/dev/null)" = hako_powerboard ] && echo "$d" && break
done)
echo "$HWMON"
```

Read fan RPM and current PWM:

```sh
cat $HWMON/fan1_input          # RPM
cat $HWMON/pwm1                # 0-255
cat $HWMON/power3_input        # microwatts
```

Set PWM directly (manual control, volatile — does not write the board's EEPROM):

```sh
echo 128 > $HWMON/pwm1         # ~50%
```

Set EEPROM-stored boot-time defaults (applied at power-on; sane fallback when the driver isn't loaded):

```sh
USB_INTF=$(realpath $HWMON/device)   # the USB interface device that owns the hwmon
echo 128 > $USB_INTF/default_pwm1
echo 128 > $USB_INTF/default_pwm2
echo 128 > $USB_INTF/default_pwm3
```

> The board firmware has no comms-loss watchdog: it holds the last `U:` value indefinitely. On graceful unload (`rmmod` / DKMS reinstall) the driver pushes your EEPROM defaults via `U:` so fans land somewhere sane. On kernel panic or hard crash, fans stay wherever they were. Set sensible `default_pwm[1-3]` values for your safety floor — typically 50–80 %.

## Integration

The driver is a generic hwmon producer; pick whatever fan-control tool you like.

### fancontrol (lm-sensors)

```sh
sudo pwmconfig                       # interactive — discovers fans/PWMs, writes /etc/fancontrol
sudo systemctl enable --now fancontrol
```

`pwmconfig` will detect the powerboard's PWM/tach pairs and let you map temperature sources to fan curves. A minimal `/etc/fancontrol` skeleton is in [`Documentation/hako-powerboard.rst`](Documentation/hako-powerboard.rst#sample-output).

### fan2go

If you use [`fan2go`](https://github.com/markusressel/fan2go), point its `hwmon` config at the device by name (`hako_powerboard`). Two gotchas worth knowing up front:

- The default `controlAlgorithm` is PID and can oscillate badly on disk-shelf fans — PWM bounces between target and 100 % every ~10 s. Add `controlAlgorithm: { direct: {} }` per fan unless you re-tune the PID gains.
- Curve `steps:` values are strings. Bare integers (`40`) mean raw PWM 0–255; percentages (`40%`) mean percent. Mixing them up looks reasonable but `40` writes 16 % PWM, usually below your fan's start-PWM, so the fan stays off.

## Uninstall

```sh
sudo make uninstall
```

Removes the udev rule and unregisters/uninstalls the DKMS module. After uninstall, `cdc_acm` will reclaim the device on the next bind event (reboot, replug, or `udevadm trigger`).

## Hardware notes worth knowing

- **One tach line per row.** Each fan wall on the powerboard exposes multiple physical 4-pin headers wired in parallel for PWM/12V/GND, but only the first physical header per row has its tach line connected back to the firmware. A 4-pin fan plugged into a non-master header will spin correctly under PWM control but always report 0 RPM. If `fan[N]_input` reads 0 with the fan visibly spinning, try moving it to the first header on that row.

- **Jumper position matters.** The board has a physical jumper that selects fan ownership. With the jumper in the motherboard position, the chassis 4-pin PWM passthrough drives fans directly and `pwm[N]` writes are silently swallowed. The driver emits a rate-limited `dmesg` warning in that case. Read `/sys/bus/usb/devices/.../jumper_mode` to check (`1` = firmware/powerboard owns PWM, `0` = motherboard owns PWM, canonicalized across hardware revisions).

- **Generic Arduino Leonardo VID:PID.** The board enumerates as `2341:8036` with no unique iSerial. The driver matches that VID:PID and discriminates Hako boards by sending `V:` in `probe()`; on parse failure it returns `-ENODEV` so `cdc_acm` gets the device back. If you have a real Arduino Leonardo on the same host, expect a brief (~1 s) delay in its `/dev/ttyACM*` appearing.

## More

- **Full sysfs reference**, hardware-revision matrix, sample output, sample `/etc/fancontrol`, troubleshooting, suspend/resume behavior: [`Documentation/hako-powerboard.rst`](Documentation/hako-powerboard.rst).
- **Design notes and wire protocol**: [`PLAN.md`](PLAN.md).
- **Changelog**: [`CHANGELOG.md`](CHANGELOG.md).

## Issues / contributing

Bug reports and pull requests welcome at [github.com/scottshanafelt/hako-powerboard-hwmon](https://github.com/scottshanafelt/hako-powerboard-hwmon). When reporting an issue, please include:

- Output of `uname -r` and your distro
- Output of `cat /sys/bus/usb/devices/<bus>-<port>:1.1/{hardware_revision,firmware_version}`
- Relevant `dmesg` output (`dmesg | grep -i hako`)

## License

GPL-2.0-or-later. Per-file SPDX identifiers are authoritative; the repo `LICENSE` file contains the canonical GPLv2 text. Kernel-module licensing constraints (the kernel exports most subsystem APIs as `EXPORT_SYMBOL_GPL`) require GPL-2.0 compatibility; the `or-later` clause permits downstream relicensing forward.
