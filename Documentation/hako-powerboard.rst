.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver hako_powerboard
=============================

Supported devices:

  * HakoForge Hako-Core Powerboard, hardware revisions 2.0, 2.1, 2.2, 2.3,
    2.4, 2.5

    Prefix:        ``hako_powerboard``

    USB IDs:       0x2341:0x8036 (interface class 0x0A — CDC Data)

Authors:

  * Scott Shanafelt <sgshanaf@gmail.com>

Description
-----------

The HakoForge Hako-Core Powerboard is a USB CDC ACM device that distributes
12V to disk backplane sections in the Hako-Core chassis and drives three
rows of 4-pin PWM fans. It exposes per-row tach feedback and four ADC shunts
for power monitoring of the 12V rails. Communication is a newline-delimited
text protocol over a USB-CDC virtual serial link at 9600 baud 8N1.

The vendor's userspace daemon (HakoForge/HakoFoundry, written in Python)
talks the board's protocol directly and computes fan curves itself. Without
this driver, nothing about the board surfaces in ``/sys/class/hwmon`` and
tools that compose with kernel hwmon (``sensors``, ``fancontrol``,
``pwmconfig``, Prometheus ``node_exporter``, Home Assistant
``systemmonitor``) cannot see the device.

This driver exposes the powerboard as a standard Linux hwmon device.
Userspace fan policy stays in userspace — the driver only exposes raw
sensors and PWM outputs.

Hardware binding
----------------

The board enumerates as a generic Arduino Leonardo (VID:PID 0x2341:0x8036)
with no unique iSerial. Two USB interfaces are exposed at enumeration: a
CDC Communications interface (subclass ACM) at index 0 and a CDC Data
interface at index 1.

The driver matches only the CDC Data interface, sends a ``V:`` query in
``probe()``, and validates the three-field ``hw,fw,location`` response. On
parse failure it returns ``-ENODEV`` so a real Arduino Leonardo on the same
host falls back to ``cdc_acm`` cleanly (after a brief delay).

Because ``cdc_acm`` matches the CDC Communications class generically and
cascade-claims the CDC Data interface internally, the kernel binds
``cdc_acm`` to the device by default at boot and on hotplug. The
``udev/99-hako-powerboard.rules`` udev rule fires on
``ACTION=="bind", DRIVER=="cdc_acm"`` for the matching VID:PID and
bInterfaceNumber 01, modprobes ``hako_powerboard``, unbinds ``cdc_acm``,
binds us. Once installed, every cdc_acm bind for this VID:PID gets the
hand-off — incompatible with HakoFoundry running concurrently.

Sysfs interface
---------------

hwmon class entries (under ``/sys/class/hwmon/hwmonN/`` with
``name == hako_powerboard``):

==========================  ===  ==========================================
Attribute                   R/W  Notes
==========================  ===  ==========================================
update_interval             RW   Per-class cache TTL in milliseconds.
                                 Range 100 .. 60000, default 1000. Lower
                                 values trade link traffic for staleness;
                                 higher values reduce the load on the
                                 9600-baud link.
fan[1-3]_input              R    RPM, row-ordered.
fan[1-3]_label              R    "Row 1" .. "Row 3".
pwm[1-3]                    RW   0–255. Driver scales to/from the board's
                                 0–100 % wire encoding. Writes are
                                 volatile (board command ``U:``); the
                                 driver never writes the EEPROM via ``F:``
                                 to avoid wear under per-second
                                 ``fancontrol`` updates.
pwm[1-3]_enable             RW   Always reads ``1`` (manual). Writes
                                 accept ``1``; writes of any other value
                                 return ``-EOPNOTSUPP``. The firmware has
                                 no closed-loop / automatic mode.
pwm[1-3]_mode               R    Always ``1`` (PWM, vs DC).
power[1-4]_input            R    Microwatts, per-rev calibration applied.
                                 See "Hardware revision matrix" below.
power[1-4]_label            R    "Shunt 1" .. "Shunt 4".
==========================  ===  ==========================================

Interface-device attrs (under
``/sys/bus/usb/devices/<bus>-<port>:1.1/``). These describe the board itself
rather than sensor channels and are not part of the standard hwmon ABI:

==========================  ===  ==========================================
Attribute                   R/W  Notes
==========================  ===  ==========================================
hardware_revision           R    From ``V:``, e.g. ``2.5``.
firmware_version            R    From ``V:``, e.g. ``2.5``.
location                    R    From ``V:``, integer multi-board
                                 disambiguator.
jumper_mode                 R    Live ``J:`` query. Canonicalized to
                                 ``1`` = firmware/powerboard owns PWM,
                                 ``0`` = motherboard owns PWM, regardless
                                 of hardware revision wire polarity.
default_pwm[1-3]            RW   Boot-time PWM values, 0–255, persisted
                                 in the board's EEPROM. Read maps to
                                 ``P:``, write maps to ``F:``. Writing
                                 also restores live PWM via a follow-up
                                 ``U:`` so running fans aren't disturbed.
                                 Equivalent to HakoFoundry's "Default Fan
                                 Speed" page. ATmega32u4 EEPROM is rated
                                 ~100k writes per cell; intended for
                                 one-time configuration, not loop writes.
==========================  ===  ==========================================

Hardware revision matrix
------------------------

The driver auto-detects HW and FW revision from the ``V:`` response at probe
time and applies the appropriate quirks and calibration:

============  ====================================  =====================
HW revision   Wattage calibration                   J: polarity (wire)
============  ====================================  =====================
2.0           linear, slope 3.574, intercept −1.375 0=fw, 1=mb (inverted)
2.1           linear, slope 3.284, intercept −1.069 0=fw, 1=mb (inverted)
2.2           multivariate (4×11 + offsets table)   0=fw, 1=mb (inverted)
2.3           multivariate (4×11 + offsets table)   0=fw, 1=mb (inverted)
2.4           linear, slope 925.25, intercept 0     0=fw, 1=mb (inverted)
2.5           linear, slope 925.25, intercept 0     1=fw, 0=mb (canonical)
============  ====================================  =====================

============  =====================
FW version    Wire-format quirk
============  =====================
2.2           ``U:`` payload sent as ``100 - row`` per channel
2.3           ``P:`` response is ``255 - x`` per channel
============  =====================

The ``jumper_mode`` sysfs attribute is canonicalized to "1 = firmware
active" regardless of the underlying wire polarity, so userspace doesn't
need to know the hardware revision.

Channel ordering
----------------

The board's wire protocol uses pin order on ``P:`` (returns ``pin1, pin2,
pin3``) and a row-with-swap order on ``U:``/``F:`` (parameters
``row2, row3, row1``). The driver normalizes everything to row order before
exposing it to userspace, so ``pwm1``/``pwm2``/``pwm3`` and
``fan1``/``fan2``/``fan3`` correspond directly to physical Row 1, Row 2,
Row 3 on the board. The pin↔row mapping is
``(row1, row2, row3) = (pin3, pin1, pin2)`` for ``P:`` reads.

Power channel safety
--------------------

The board firmware has no comms-loss watchdog and no DTR-driven fallback —
it holds whatever ``U:`` value was last written indefinitely (verified
empirically). To prevent fans being stranded at unsafe values when the
driver unloads, ``hako_disconnect()`` reads the EEPROM defaults via ``P:``
and pushes them via ``U:`` before deasserting DTR. Set
``default_pwm[1-3]`` to a sane safety value (typically 50 % to 80 %) to
benefit from this.

The disconnect-time push relies on ``usb_driver.soft_unbind = 1`` to keep
the USB endpoints active across the unbind window. If the device is
genuinely disconnected (USB unplug), the kernel ignores the hint and
terminates URBs anyway; the safety push degrades to fast-failure with
``-ENODEV``, which is acceptable since the device is gone.

Jumper mode
-----------

The powerboard has a physical jumper that selects fan ownership:

* Firmware position (``jumper_mode == 1``): the firmware drives the fan
  PWM output via ``U:``. Driver writes to ``pwm[N]`` are effective.

* Motherboard position (``jumper_mode == 0``): the chassis 4-pin PWM
  passthrough drives fans directly. ``U:`` writes are accepted by the
  firmware but the hardware ignores them — fans don't track ``pwm[N]``
  writes.

When ``pwm[N]`` is written and the cached jumper state says motherboard,
the driver emits a rate-limited warning to ``dmesg``::

  hako_powerboard X-Y:1.1: pwm write while motherboard owns the jumper
  — writes won't reach the fans

The cache is sampled at probe and refreshed opportunistically when
``jumper_mode`` is read; if you toggle the physical jumper while the
driver is loaded, ``cat .../jumper_mode`` to refresh, or ``rmmod`` +
``modprobe`` to re-probe.

Sample output
-------------

``sensors`` after ``modprobe hako_powerboard``::

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

A minimal ``/etc/fancontrol`` snippet (run ``pwmconfig`` for an interactive
generator)::

  INTERVAL=10
  DEVPATH=hwmon5=devices/pci0000:00/0000:00:14.0/usb1/X-Y/X-Y:1.1
  DEVNAME=hwmon5=hako_powerboard
  FCTEMPS=hwmon5/pwm1=hwmon4/temp1_input
  FCFANS=hwmon5/pwm1=hwmon5/fan1_input
  MINTEMP=hwmon5/pwm1=40
  MAXTEMP=hwmon5/pwm1=70
  MINSTART=hwmon5/pwm1=64
  MINSTOP=hwmon5/pwm1=32

The hwmonN device path will differ between machines; use ``ls -l
/sys/class/hwmon/`` to identify the entry whose ``name`` reads
``hako_powerboard``.

Troubleshooting
---------------

**Driver fails to probe / dmesg shows "V: query failed: -110".**
The Arduino's CDC firmware ignores serial input until DTR is asserted.
The driver asserts DTR via ``SET_CONTROL_LINE_STATE`` on the CDC Comm
interface in probe; if a different driver had the comm interface and
deasserted DTR shortly before, there can be a brief settling delay. Try
``rmmod hako_powerboard && modprobe hako_powerboard``.

**Boot dmesg shows ``cdc_acm 1-N:1.0: probe with driver cdc_acm failed with error -16``.**
Benign. ``-16`` is ``-EBUSY``: ``hako_powerboard`` registered first and
already owns the CDC Data interface (``1.1``). When ``cdc_acm`` then
probes the CDC Comm interface (``1.0``) it tries to cascade-claim
``1.1`` too, fails with ``-EBUSY``, and aborts. Confirm via
``readlink /sys/bus/usb/devices/<bus>-<port>:1.1/driver`` — should
point at ``hako_powerboard``. The Comm interface (``1.0``) is left
unbound on purpose; the driver issues ``SET_CONTROL_LINE_STATE`` via
control transfer using the cached comm-interface number and does not
need a bind there. This is the "we got there first" happy path; the
udev rule is the fallback for the reverse race.

**Driver fails to probe / dmesg shows "bulk endpoints not found".**
Indicates the driver was bound to the CDC Comm interface (1.0) instead of
the CDC Data interface (1.1). This happens when a dynamic id was added
via ``echo VID PID > new_id`` — the wildcard match bypasses the static
``USB_DEVICE_AND_INTERFACE_INFO`` interface-class filter. The probe-time
``bInterfaceClass`` check returns ``-ENODEV`` silently in this case so
the kernel can re-probe a different interface; nothing actually broke.

**``pwm[N]`` writes don't move the fans.**
Check ``cat .../jumper_mode``. A reading of ``0`` means the motherboard
owns the 4-pin PWM signal and your writes are silently swallowed at the
hardware level. Move the physical jumper or reconnect the chassis 4-pin
header to the powerboard.

**``cat pwm1`` returns 127 forever after ``echo 200 > pwm1``.**
This was a v0.1 bug; v0.2+ caches the live ``U:`` value after writes
because ``P:`` returns the EEPROM-stored value, not the live PWM.

**HakoFoundry can't open ``/dev/ttyACM0`` after installing the udev rule.**
Expected. The udev rule pulls the data interface away from ``cdc_acm``,
so ``cdc_acm`` doesn't register a tty for this device. Treat udev rule
installation as a one-way migration step. To revert, remove
``/etc/udev/rules.d/99-hako-powerboard.rules`` (or run ``sudo make
uninstall-udev``), reload udev, and unbind/rebind the data interface.

**Fans stuck at the wrong speed after ``rmmod``.**
Check whether ``default_pwm[1-3]`` is set to a sensible value before
unloading. The driver's safety push only restores those EEPROM-stored
defaults; if you set ``default_pwm[*]`` to 0 % the fans will land at 0
on disconnect. Recovery: ``modprobe hako_powerboard`` and write a sane
``pwm[N]`` value.

**``fan[N]_input`` reads 0 even with a 4-pin fan spinning.**
Each fan wall on the powerboard exposes multiple physical 4-pin
headers wired in parallel for PWM/12V/GND, but only one header per
row has its tach line connected back to the firmware (typically the
first physical header in the row). Plug the fan whose RPM you want to
read into the first header on that row; the other headers will
silently report 0 RPM regardless of fan speed. Verified on Hako-Core
Mini HW 2.5; behavior is hardware, not driver, so it applies to
HakoFoundry as well.

Limitations
-----------

* HW 2.2 / 2.3 wattage replicates Foundry's ``_calculate_wattage_22`` —
  4 × 11 cross-shunt regression with an empirical post-correction lookup
  table. Output matches Foundry's float reference exactly across a
  battery of test vectors.

* ``pwm[1-3]_label`` is intentionally absent: ``HWMON_PWM_LABEL`` is not
  in the Linux 6.19 ``linux/hwmon.h`` enum. ``fan[1-3]_label`` carries
  the row labels; userspace tooling that wants pwm labels can fall back
  to that.

* The board's ``F:`` and ``U:`` response formats are undocumented; the
  driver waits ~500 ms for any echo and ignores the body.

* The driver does not implement a ``thermal_cooling_device`` binding;
  use ``fancontrol``, ``thermald``, or another userspace fan-policy
  tool to drive ``pwm[N]`` from temperature inputs.

Suspend / resume
----------------

The driver implements ``.suspend``, ``.resume``, and ``.reset_resume``
on the USB driver. On suspend the RX URB is killed so the host stops
polling; on resume the URB is resubmitted, DTR is re-asserted (the
Arduino's CDC firmware treats USB suspend as a DTR transition), and the
fan / power / pwm caches are invalidated so the next hwmon poll re-reads
fresh values from the device. Any ``hako_query`` waiter that was in
flight at suspend time ends up timing out with ``-ETIMEDOUT``.
