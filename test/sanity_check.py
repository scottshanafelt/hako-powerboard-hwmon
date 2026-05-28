#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Assert-based sanity check for the hako_powerboard driver.

Unlike read_hwmon.py (which just dumps attributes), this script makes pass/fail
assertions about the three sensor classes and exits non-zero on any failure, so
it is suitable for a post-install smoke gate or CI on the test box.

Checks:
  * RPM   - fan[1-3]_input readable, integer, >= 0
  * PWM   - pwm[1-3] readable, integer, 0..255; plus a write/read-back round
            trip on one channel (needs root) that confirms the U:-write cache
            path returns exactly what was written, then restores the original
  * POWER - power[1-4]_input readable, integer, >= 0 (microwatts); warns (does
            not fail) if every shunt reads 0, since that may just be an idle rail

Run as root to include the PWM round-trip:  sudo ./sanity_check.py
"""

import os
import sys
import time
from pathlib import Path

HWMON_ROOT = Path("/sys/class/hwmon")
NUM_FAN = 3
NUM_PWM = 3
NUM_POWER = 4

# ANSI is avoided so output stays clean in logs / over ssh.
_passes = 0
_failures = []
_warnings = []


def ok(msg):
    global _passes
    _passes += 1
    print(f"  PASS  {msg}")


def fail(msg):
    _failures.append(msg)
    print(f"  FAIL  {msg}")


def warn(msg):
    _warnings.append(msg)
    print(f"  WARN  {msg}")


def find_hako():
    for entry in sorted(HWMON_ROOT.glob("hwmon*")):
        try:
            if (entry / "name").read_text().strip() == "hako_powerboard":
                return entry
        except OSError:
            continue
    return None


def read_int(path):
    """Return (value, None) on success or (None, error_string)."""
    try:
        return int(path.read_text().strip()), None
    except OSError as e:
        return None, e.strerror
    except ValueError as e:
        return None, str(e)


def check_range(label, path, lo, hi):
    val, err = read_int(path)
    if err is not None:
        fail(f"{label}: not readable ({err})")
        return None
    if not (lo <= val <= hi):
        fail(f"{label}: {val} out of range [{lo}, {hi}]")
        return None
    ok(f"{label}: {val}")
    return val


def check_rpm(hwmon):
    print("\n[RPM]")
    for i in range(1, NUM_FAN + 1):
        # Upper bound is generous; real fans top out well under this.
        check_range(f"fan{i}_input", hwmon / f"fan{i}_input", 0, 100000)


def check_power(hwmon):
    print("\n[POWER]")
    vals = []
    for i in range(1, NUM_POWER + 1):
        # 12 V rail, well under 50 A per shunt -> < 600 W -> 6e8 uW ceiling.
        v = check_range(f"power{i}_input", hwmon / f"power{i}_input", 0, 600_000_000)
        if v is not None:
            vals.append(v)
    if vals and all(v == 0 for v in vals):
        warn("all four shunts read 0 uW - rail idle, or calibration suspect")


def check_pwm(hwmon):
    print("\n[PWM]")
    for i in range(1, NUM_PWM + 1):
        check_range(f"pwm{i}", hwmon / f"pwm{i}", 0, 255)

    if os.geteuid() != 0:
        warn("not root - skipping pwm write/read-back round trip")
        return

    # Round-trip on channel 1: write a distinct value, confirm the read-back
    # cache returns it exactly, then restore. The driver's U: write makes its
    # cache authoritative for pwm reads, so the read-back must match exactly.
    path = hwmon / "pwm1"
    orig, err = read_int(path)
    if err is not None:
        fail(f"pwm1 round trip: cannot read original ({err})")
        return

    test_val = 150 if orig != 150 else 100
    try:
        path.write_text(f"{test_val}\n")
    except OSError as e:
        fail(f"pwm1 round trip: write failed ({e.strerror})")
        return

    time.sleep(0.3)
    readback, err = read_int(path)
    if err is not None:
        fail(f"pwm1 round trip: read-back failed ({err})")
    elif readback != test_val:
        fail(f"pwm1 round trip: wrote {test_val}, read {readback}")
    else:
        ok(f"pwm1 round trip: wrote and read back {test_val}")

    # Restore original regardless of outcome.
    try:
        path.write_text(f"{orig}\n")
        time.sleep(0.3)
        restored, _ = read_int(path)
        if restored == orig:
            ok(f"pwm1 restored to {orig}")
        else:
            warn(f"pwm1 restore: expected {orig}, got {restored}")
    except OSError as e:
        warn(f"pwm1 restore failed ({e.strerror}) - left at {test_val}")


def main():
    hwmon = find_hako()
    if not hwmon:
        print("hako_powerboard not found under /sys/class/hwmon", file=sys.stderr)
        print("  is the driver loaded?  lsmod | grep hako_powerboard", file=sys.stderr)
        return 1

    print(f"hwmon path: {hwmon}  (name={(hwmon / 'name').read_text().strip()})")
    check_rpm(hwmon)
    check_pwm(hwmon)
    check_power(hwmon)

    print("\n[SUMMARY]")
    print(f"  {_passes} passed, {len(_failures)} failed, {len(_warnings)} warnings")
    if _failures:
        print("  FAILURES:")
        for f in _failures:
            print(f"    - {f}")
        return 1
    print("  OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
