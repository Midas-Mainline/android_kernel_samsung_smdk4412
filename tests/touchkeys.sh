#!/bin/sh
set -e
cat /sys/class/leds/tm2-touchkey/max_brightness > \
    /sys/class/leds/tm2-touchkey/brightness
evtest /dev/input/event0
