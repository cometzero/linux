#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

control=/sys/kernel/debug/gic720ae_test/control
module=/lib/modules/"$(uname -r)"/extra/gic720ae_test.ko
cpu_online=/sys/devices/system/cpu/cpu1/online

cleanup()
{
	if [ -e "$cpu_online" ]; then
		printf '1\n' >"$cpu_online" || true
	fi
	rmmod gic720ae_test || true
}

mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
insmod "$module" target_cpu=1
trap cleanup EXIT HUP INT TERM

printf 'ipi 1\n' >"$control"
printf 'spi 1\n' >"$control"
printf 'snapshot\n' >"$control"
cat /proc/interrupts
printf '0\n' >"$cpu_online"
if printf 'ipi 1\n' >"$control" 2>/dev/null; then
	exit 1
fi
if printf 'spi 1\n' >"$control" 2>/dev/null; then
	exit 1
fi
printf '1\n' >"$cpu_online"
printf 'snapshot\n' >"$control"
printf 'ipi 1\n' >"$control"
printf 'spi 1\n' >"$control"
cat /proc/interrupts
