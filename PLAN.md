# Hako Powerboard Linux hwmon Driver — Design Notes

This file captures the design rationale, wire-protocol facts, and hardware quirks the driver encodes. End-user docs live in `README.md` and `Documentation/hako-powerboard.rst`. This file is for contributors and anyone trying to understand *why* the driver does what it does.

## Context

The HakoForge Hako-Core Powerboard is a USB CDC ACM device that distributes 12V to disk backplanes and drives three rows of 4-pin PWM fans. The vendor's userspace daemon (HakoForge/HakoFoundry, Python) talks the board's serial protocol and computes fan curves itself, exposing nothing to the standard Linux sensor stack. Concretely:

- No `/sys/class/hwmon` entries for the powerboard, so `sensors`/`fancontrol`/Prometheus `node_exporter`/HA `systemmonitor` see nothing
- Foundry must own fan policy; users can't substitute `fancontrol` or thermald
- Tools that compose with kernel hwmon don't compose at all here

This driver exposes the powerboard as a standard Linux hwmon device. v0.2.0 ships fan/tach/PWM, 4× shunt wattage, EEPROM-default PWM, and device metadata. Packaged as DKMS; eventual upstream submission is a longer-term goal. License: GPL-2.0-or-later (per-file SPDX); `MODULE_LICENSE("GPL v2")`.

## Approach

Standard **USB driver** with `usb_device_id` table matching the powerboard's VID/PID, plus probe-time content validation to discriminate from non-Hako Arduino Leonardos. The hwmon side uses the modern `hwmon_chip_info` API (no legacy `SENSOR_DEVICE_ATTR` macros). Single C file, ~600–800 LOC.

Modeled on three in-tree drivers:
- `drivers/hwmon/aquacomputer_d5next.c` — primary template for `hwmon_chip_info` with multi-channel pwm + fan, labels via `hwmon_ops.read_string`
- `drivers/hwmon/corsair-psu.c` — request/response over a slow link with jiffies-based staleness cache + mutex-serialized transactions
- `drivers/iio/chemical/sps30_serial.c` — newline-delimited text protocol with completion-based response wait (note: serdev-based; we use direct USB bulk URBs instead, but the line-assembly + completion pattern transfers)

## Hardware & protocol (confirmed from `powerboard.py` + live capture)

|Fact|Value|
|-|-|
|Transport|USB CDC ACM, 9600 baud 8N1|
|VID/PID|`2341:8036` (generic Arduino Leonardo, no unique iSerial)|
|Framing|`<CMD>:<params>\n` request → single `\n`-terminated comma-separated response|
|Concurrency|Strictly serial; one transaction in flight|
|Channels|3× PWM out, 3× tach in, 4× ADC for shunts, 1× jumper|

Commands:

|Cmd|Purpose|Wire format|Notes|
|-|-|-|-|
|`V:`|Metadata|→ `hw_rev,fw_ver,location`|Read at probe; controls quirks; first field shape `<digit>.<digit>` validates "is a Hako board"|
|`P:`|Read PWM|→ 3 ints 0-255|FW 2.3 inverts (board returns `255-x`)|
|`T:`|Read tach|→ 3 ints, ×30 for RPM|Order matches rows|
|`W:`|Read ADC|→ 4 raw ints (signed)|v0.2; HW 2.4+ (incl 2.5) use slope=925.25, intercept=0; HW 2.2/2.3 use multivariate regression in HakoFoundry — kernel driver skips multivariate, supports linear-only|
|`F:`|Set PWM (EEPROM)|`row2,row3,row1` 0-100|**Driver does not use this** — wear|
|`U:`|Set PWM (volatile)|`row2,row3,row1` 0-100|FW 2.2 inverts (sends `100-x`)|
|`J:`|Read jumper|→ `0` or `1`|HW 2.5 polarity inverted (see quirks)|

Quirks the driver must encode at probe based on `V:`:
- FW 2.2: `U:` payload is `100 - row` per channel
- FW 2.3: `P:` response is `255 - x` per channel
- **HW 2.5: `J:` polarity is inverted vs older revs.** Pre-2.5: `J:=0` = powerboard/firmware, `J:=1` = motherboard. HW 2.5: `J:=1` = firmware, `J:=0` = motherboard. Foundry's source treats J as if it were always pre-2.5 polarity until commit `619855`. Driver canonicalizes `jumper_mode` sysfs attr to "1 = firmware/powerboard active" regardless of hw_rev.
- All revs: command parameter order is `row2,row3,row1` (pin↔row swap); `T:` is already in row order; `P:` is in pin order and Foundry rearranges as `(pin3, pin1, pin2) → (row1, row2, row3)`. Driver exposes **row order** as `pwm1`/`pwm2`/`pwm3` externally.

## v0.1 sysfs attribute map

Under the hwmon device:

|Attribute|Backing|R/W|Notes|
|-|-|-|-|
|`name`|static "hako_powerboard"|R|Required|
|`pwm1`–`pwm3`|`P:` read, `U:` write|RW|0–255 on the sysfs side; driver scales to/from 0–100 percent on the wire|
|`pwm[1-3]_enable`|fixed `1` (manual)|R|Hwmon convention; firmware has no closed-loop|
|`pwm[1-3]_mode`|fixed `1` (PWM)|R|Hwmon convention|
|`fan1_input`–`fan3_input`|`T:` × 30|R|RPM|
|`fan[1-3]_label`|"Row 1".."Row 3"|R||
|`pwm[1-3]_label`|"Row 1".."Row 3"|R|Optional but useful|

Non-standard device attributes (under `/sys/bus/.../`, not the hwmon class):

|Attribute|Backing|R/W|Notes|
|-|-|-|-|
|`hardware_revision`|`V:` field 1|R|e.g. "2.5"|
|`firmware_version`|`V:` field 2|R|e.g. "2.5"|
|`location`|`V:` field 3|R|Multi-board disambiguation|
|`jumper_mode`|`J:` (canonicalized)|R|1 = firmware/powerboard, 0 = motherboard (after hw_rev-aware inversion)|

`pwm[1-3]` writes when `jumper_mode == 0` (motherboard owns PWM): log a single rate-limited warning via `dev_warn_ratelimited()` but accept the write (matches lm_sensors expectations; rejecting with `-EBUSY` would break `pwmconfig`).

## USB binding strategy (VID/PID + probe-time discrimination)

The test powerboard enumerates as a stock Arduino Leonardo: VID/PID `2341:8036`, `iManufacturer="Arduino LLC"`, `iProduct="Arduino Leonardo"`, `iSerial=""` (firmware does not populate it). Currently bound by `cdc_acm`.

**Strategy:** USB driver with `USB_DEVICE_AND_INTERFACE_INFO(0x2341, 0x8036, USB_CLASS_CDC_DATA, 0, 0)` so we only probe interface 1 (the CDC data interface with the bulk endpoints). Probe procedure:

1. Find bulk in/out endpoints on the interface
2. Allocate RX URB; receive callback assembles bytes into a line buffer, signals a `completion` on `\n`
3. Submit RX URB, then send `V:` synchronously via `usb_bulk_msg()`
4. Wait on completion (2 s timeout)
5. Validate response: three comma-separated fields, first matches `<digit>.<digit>` (HW rev), location parses as integer
6. If valid → parse hw_rev/fw_ver/location into `struct hako_data`, set quirks bitfield, register hwmon
7. If invalid or timeout → return `-ENODEV`; kernel hands the device back to `cdc_acm` so a real Arduino Leonardo sketch keeps working

This pattern is standard upstream (FTDI/CH340/CP210x sub-drivers do similar discrimination). It also handles **multiple powerboards on one host natively** — each device gets its own `probe()` call and its own `/sys/class/hwmon/hwmonN/` entry. The `V:` `location` field becomes part of each hwmon device's labels (`PB1 Row 1`, `PB2 Row 1`, etc.) so userspace can tell them apart.

A small `modprobe.d` config or udev `new_id` write may be needed at first install to make our driver win the race against `cdc_acm` for already-enumerated devices; on cold boot, module load order suffices.

**Future improvement** (not blocking v0.1): if HakoForge ever ships firmware that populates `iSerial` with a unique per-board string, the driver could match on serial — cleaner and survives accidental Arduino Leonardo collisions.

## File layout

Existing repo root. Target structure (scaffold complete):

```
hako-powerboard-hwmon/
  src/
    hako_powerboard.c     # ~600-800 LOC, single file, SPDX GPL-2.0-or-later
    Makefile              # obj-m := hako_powerboard.o
  dkms.conf
  udev/
    99-hako-powerboard.rules
  modprobe.d/
    hako-powerboard.conf  # only if needed for cdc-acm disambiguation
  test/
    read_hwmon.py         # walks /sys/class/hwmon, prints labeled values
    set_pwm.sh            # writes pwm1/2/3, reads back fan*_input
  README.md
  PLAN.md                 # this file
  LICENSE                 # canonical GPLv2 text
```

Single-file internal organization (`src/hako_powerboard.c`):
1. SPDX header, includes, MODULE_* macros
2. `struct hako_data` — usb_device, usb_interface, mutex, RX URB, RX line buffer, completion, response buffer, cached values, last_update jiffies, fw_quirks bitfield
3. RX path: bulk URB completion handler → byte-by-byte line assembly → on `\n`, copy to response buffer + `complete()`
4. TX helpers: `hako_send_cmd()` (synchronous via `usb_bulk_msg()`), `hako_query()` (TX + `wait_for_completion_timeout`, mutex-serialized)
5. Refresh helpers: `hako_refresh_pwm()`, `_tach()` with jiffies-based 1 s staleness check
6. hwmon ops: `is_visible`, `read`, `write`, `read_string` (labels)
7. `hwmon_chip_info` + `hwmon_channel_info` arrays
8. Device attrs (hardware_revision, firmware_version, location, jumper_mode)
9. USB probe/disconnect: claim CDC data interface, find bulk endpoints, allocate URB, send `V:`, parse + quirks, register hwmon

Polling/locking:
- Single `struct mutex xfer_lock` serializes all serial transactions
- Per-class cache (`pwm`, `tach`) with `unsigned long last_update` jiffies; staleness window 1000 ms (`HZ`)
- On hwmon `.read`: if stale, query via `xfer_lock`, update cache, return; else return cached value
- On hwmon `.write` (pwm): take `xfer_lock`, scale 0-255→percent, apply quirks, swap row order, send `U:`, invalidate pwm cache
- No background `delayed_work` — purely on-demand

DKMS:

```
PACKAGE_NAME="hako-powerboard"
PACKAGE_VERSION="0.1.0"
BUILT_MODULE_NAME[0]="hako_powerboard"
DEST_MODULE_LOCATION[0]="/kernel/drivers/hwmon"
AUTOINSTALL="yes"
MAKE[0]="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build/src modules"
CLEAN="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build/src clean"
```

## Build sequence

The driver was built up incrementally; the v0.2.0 release encompasses all of these:

1. **USB skeleton.** `USB_DEVICE_AND_INTERFACE_INFO(0x2341, 0x8036, USB_CLASS_CDC_DATA, 0, 0)`, bulk endpoint discovery via `usb_find_common_endpoints()`, RX URB with byte-by-byte line assembly on `\n`, mutex-serialized `hako_query()` (TX `usb_bulk_msg`, RX `wait_for_completion_timeout`).
2. **`V:` probe with two non-obvious gotchas.**
   - **Static id_table interface-class filter is bypassed by `new_id` writes.** `USB_DEVICE_AND_INTERFACE_INFO` filters to `USB_CLASS_CDC_DATA` via `match_flags = MATCH_DEVICE | MATCH_INT_INFO`, but `echo VID PID > new_id` adds a dynamic id with `match_flags = MATCH_DEVICE` only — the interface-class filter doesn't apply. Probe must therefore re-check `bInterfaceClass == USB_CLASS_CDC_DATA` and return `-ENODEV` silently if it's wrong.
   - **Arduino's CDC firmware ignores serial input until DTR is asserted.** `cdc-acm` does this on tty open via `SET_CONTROL_LINE_STATE`; raw drivers have to replicate it. Without DTR, `V:` TX succeeds but the Arduino app loop drops the bytes and `hako_query` times out. Probe issues `usb_control_msg(SET_CONTROL_LINE_STATE, DTR|RTS)` to the CDC Comm interface, with a 100 ms settle, before any query. Disconnect deasserts so `cdc-acm`'s next bind sees a clean transition.
3. **Read-only hwmon**: tach channels, then pwm readback. `sensors` showing values is the milestone.
4. **PWM write**: full RW pwm channels with the row/percent/inversion transforms; cache invalidation on write; DMA-safe TX scratch buffer for `usb_bulk_msg`.
5. **Cache + locking polish**: `xfer_lock`, jiffies-based per-class staleness, error paths (timeout, malformed line, RX overrun), `dev_warn_ratelimited` on `jumper_mode=0` writes.
6. **Device attrs**: `hardware_revision`, `firmware_version`, `location`, `jumper_mode` (canonicalized across hw revs), and `default_pwm[1-3]` (RW EEPROM-stored boot defaults).
7. **DKMS packaging** + udev auto-rebind rule.
8. **Disconnect-time safety push**: read EEPROM defaults via `P:`, push them via `U:` so fans land at the user's configured safe state on rmmod / DKMS reinstall. Requires `usb_driver.soft_unbind = 1`.
9. **Wattage support**: per-rev linear calibration for HW 2.0 / 2.1 / 2.4+; multivariate regression for HW 2.2 / 2.3 matching HakoFoundry's `_calculate_wattage_22` exactly. Pure integer math.
10. **Suspend/resume**: kill RX URB on suspend; resubmit + reassert DTR + invalidate caches on resume.
11. **Tunable `update_interval`** (chip-level RW attr, 100..60000 ms) and `power[1-4]_label` polish.

Open longer-term: `pwm[1-3]_label` (depends on `HWMON_PWM_LABEL` enum, not yet in the kernels we target); thermal-cooling-device class binding (out of scope — userspace fan policy is the design); upstream submission.

## Verification

1. Verify the device is bound to our driver: `ls /sys/bus/usb/drivers/hako_powerboard/`. Probe message in `dmesg` reads `Hako-Core Powerboard hw <X.Y> fw <X.Y> location <N> (quirks 0x<bits>)`.
2. `ls /sys/class/hwmon/` → identify `hwmonN` whose `name` reads `hako_powerboard`.
3. `cat /sys/class/hwmon/hwmonN/{fan1_input,fan2_input,fan3_input,pwm1,pwm2,pwm3}` returns plausible values.
4. `sensors` lists the device with row and shunt labels.
5. `echo 128 > /sys/class/hwmon/hwmonN/pwm1` → wait 2 s → `cat fan1_input` shows RPM change.
6. `cat /sys/bus/usb/devices/<bus>-<port>:1.1/jumper_mode` returns `1` for firmware-active or `0` for motherboard-active (canonical across hw revs).
7. `pwmconfig` interactive run completes; generated `/etc/fancontrol` references our PWMs; `systemctl start fancontrol` and confirm thermal sources drive fan speeds.
8. `dkms install hako-powerboard/0.2.0`; reboot; confirm module auto-loads, hwmon entries reappear, fancontrol starts, fans respond.

## Recon findings

|Item|Result|
|-|-|
|VID/PID|`2341:8036` (Arduino Leonardo, generic) — no unique iSerial|
|`V:`|`2.5,2.5,1\n` — hw rev **2.5**, fw 2.5, location 1|
|`P:`|`0,127,127\n` — 3 ints 0-255, pin order|
|`T:`|`49,0,0\n` — 3 ints, row order, ×30 = RPM (1470, 0, 0)|
|`W:`|`-20,7,1510,-29\n` — 4 raw ADC ints, signed|
|`J:`|`1\n` — on HW 2.5 = firmware/powerboard control (jumper polarity inverted vs older revs)|
|Framing|LF-terminated (not CRLF), no echo, no leading whitespace|
|Boot bytes|None after 2.5s post-open — DTR reset either didn't fire or completed silently. No long warm-up needed in probe.|
|Multi-board|Single board on the test host|

## Hardware-quirk rationale

- **HW 2.5 wattage calibration.** HakoFoundry's catch-all linear calibration is `slope=925.25, intercept=0`. But Foundry routes HW 2.2 and HW 2.3 through `_calculate_wattage_22` (multivariate) regardless of the per-rev slope/intercept, so the linear `925.25/0` formula effectively applies to HW 2.4+ (incl 2.5) only. Linear formula: `current_A = max(0, raw_adc / 925.25)`, `power_uW = current_A * 12 * 1_000_000` — pure integer math in the driver.
- **HW 2.5 jumper polarity.** The jumper wire meaning splits by hw rev: HW 2.5 wire `J:=1` = Powerboard, `J:=0` = Motherboard; pre-2.5 wire is opposite. Driver canonicalizes the `jumper_mode` sysfs attr to "1 = firmware/powerboard active" so userspace doesn't have to know the rev. Quirk bit `HAKO_Q_J_INVERT_PRE25` fires on pre-2.5 hardware (action-oriented direction — set when we need to invert).
- **License compatibility.** GPL-2.0-or-later is kernel-loadable (the kernel sees a GPLv2 face) and allows downstream relicensing forward. GPL-3.0 alone is *not* kernel-module-loadable for code that uses `EXPORT_SYMBOL_GPL` symbols, which the hwmon and USB APIs are.
