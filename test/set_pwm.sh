#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Sweep pwm[1-3] across a few duty cycles and observe fan*_input response.
#
# Usage: sudo ./set_pwm.sh [/sys/class/hwmon/hwmonN]
#
# If the path argument is omitted, auto-discovers the hako_powerboard hwmon
# directory. Restores the original PWM values on exit.

set -eu

find_hwmon() {
    for d in /sys/class/hwmon/hwmon*; do
        if [ -r "$d/name" ] && [ "$(cat "$d/name")" = hako_powerboard ]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

HWMON="${1:-$(find_hwmon || true)}"
if [ -z "${HWMON:-}" ] || [ ! -d "$HWMON" ]; then
    echo "could not locate hako_powerboard under /sys/class/hwmon" >&2
    echo "  is the driver loaded?  lsmod | grep hako_powerboard" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root (PWM writes need write access to sysfs)" >&2
    exit 1
fi

echo "using $HWMON"

# Snapshot starting PWM values; restore on exit (clean or error).
PWM1_ORIG=$(cat "$HWMON/pwm1")
PWM2_ORIG=$(cat "$HWMON/pwm2")
PWM3_ORIG=$(cat "$HWMON/pwm3")

restore() {
    echo "restoring pwm1=$PWM1_ORIG pwm2=$PWM2_ORIG pwm3=$PWM3_ORIG"
    echo "$PWM1_ORIG" > "$HWMON/pwm1" 2>/dev/null || true
    echo "$PWM2_ORIG" > "$HWMON/pwm2" 2>/dev/null || true
    echo "$PWM3_ORIG" > "$HWMON/pwm3" 2>/dev/null || true
}
trap restore EXIT INT TERM

printf '%-6s %-8s %-8s %-8s\n' "pwm" "fan1" "fan2" "fan3"
for v in 0 64 128 192 255; do
    echo "$v" > "$HWMON/pwm1"
    echo "$v" > "$HWMON/pwm2"
    echo "$v" > "$HWMON/pwm3"
    sleep 3
    f1=$(cat "$HWMON/fan1_input")
    f2=$(cat "$HWMON/fan2_input")
    f3=$(cat "$HWMON/fan3_input")
    printf '%-6s %-8s %-8s %-8s\n' "$v" "$f1" "$f2" "$f3"
done
