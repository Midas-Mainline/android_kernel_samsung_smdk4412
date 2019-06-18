#!/bin/sh
watch -n1 "cat /sys/class/power_supply/max77693-charger/uevent"  > /dev/tty0
