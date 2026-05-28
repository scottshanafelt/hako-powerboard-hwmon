#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Scan the kernel log for hako_powerboard messages and fail if any look like
# an error/warning. The driver's normal lines (probe banner, driver register,
# disconnect) contain none of the flagged keywords, so a clean run prints only
# the expected lines and exits 0.
#
# Usage: sudo ./check_dmesg.sh
#   (dmesg often needs root; without it the script warns and skips.)

set -u

if ! dmesg >/dev/null 2>&1; then
    echo "WARN  cannot read dmesg (try sudo); skipping log scan"
    exit 0
fi

HAKO_LINES=$(dmesg | grep -i hako_powerboard || true)

if [ -z "$HAKO_LINES" ]; then
    echo "WARN  no hako_powerboard lines in dmesg - is the driver loaded?"
    exit 0
fi

echo "hako_powerboard kernel log lines:"
echo "$HAKO_LINES" | sed 's/^/  /'

# Keywords that should never appear in a healthy run.
SUSPICIOUS=$(echo "$HAKO_LINES" | grep -iE \
    'overrun|fail|error|timed out|timeout| -E[A-Z]|taint|warn|BUG|stack|oops|refus' \
    || true)

echo
if [ -n "$SUSPICIOUS" ]; then
    echo "FAIL  suspicious kernel log lines found:"
    echo "$SUSPICIOUS" | sed 's/^/    /'
    exit 1
fi

echo "PASS  no suspicious hako_powerboard log lines"
exit 0
