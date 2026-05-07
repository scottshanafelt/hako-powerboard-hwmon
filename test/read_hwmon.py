#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Walk /sys/class/hwmon for hako_powerboard and pretty-print all attributes.

Use this as a post-install smoke test — confirms the driver registered and
that fan/pwm/power channels and device-metadata attrs are readable.
"""

import sys
from pathlib import Path

HWMON_ROOT = Path("/sys/class/hwmon")


def find_hako():
    for entry in sorted(HWMON_ROOT.glob("hwmon*")):
        try:
            if (entry / "name").read_text().strip() == "hako_powerboard":
                return entry
        except OSError:
            continue
    return None


def read_attr(path):
    try:
        return path.read_text().strip()
    except OSError as e:
        return "<error: {}>".format(e.strerror)


def print_section(title, attrs):
    print("\n{}".format(title))
    print("-" * len(title))
    for label, path in attrs:
        if path.exists():
            print("  {:<24} {}".format(label, read_attr(path)))


def main():
    hwmon = find_hako()
    if not hwmon:
        print("hako_powerboard not found under /sys/class/hwmon", file=sys.stderr)
        print("  - is the driver loaded?     lsmod | grep hako_powerboard", file=sys.stderr)
        print("  - is the device bound?      ls /sys/bus/usb/drivers/hako_powerboard/", file=sys.stderr)
        return 1

    print(f"hwmon path: {hwmon}")
    print(f"name:       {read_attr(hwmon / 'name')}")

    fan_attrs = []
    for i in range(1, 4):
        fan_attrs.append((f"fan{i}_label", hwmon / f"fan{i}_label"))
        fan_attrs.append((f"fan{i}_input (RPM)", hwmon / f"fan{i}_input"))
    print_section("Fan tach", fan_attrs)

    pwm_attrs = []
    for i in range(1, 4):
        pwm_attrs.append((f"pwm{i} (0-255)", hwmon / f"pwm{i}"))
        pwm_attrs.append((f"pwm{i}_enable", hwmon / f"pwm{i}_enable"))
        pwm_attrs.append((f"pwm{i}_mode", hwmon / f"pwm{i}_mode"))
    print_section("PWM", pwm_attrs)

    power_attrs = []
    for i in range(1, 5):
        power_attrs.append((f"power{i}_label", hwmon / f"power{i}_label"))
        power_attrs.append((f"power{i}_input (uW)", hwmon / f"power{i}_input"))
    print_section("Power (12 V shunts)", power_attrs)

    print_section("Chip", [("update_interval (ms)", hwmon / "update_interval")])

    # Interface-device attrs sit alongside, not under, the hwmon class entry.
    intf_dev = (hwmon / "device").resolve()
    if intf_dev.exists():
        intf_attrs = [
            ("hardware_revision", intf_dev / "hardware_revision"),
            ("firmware_version", intf_dev / "firmware_version"),
            ("location", intf_dev / "location"),
            ("jumper_mode", intf_dev / "jumper_mode"),
            ("default_pwm1", intf_dev / "default_pwm1"),
            ("default_pwm2", intf_dev / "default_pwm2"),
            ("default_pwm3", intf_dev / "default_pwm3"),
        ]
        print_section(f"Device metadata ({intf_dev})", intf_attrs)

    return 0


if __name__ == "__main__":
    sys.exit(main())
