#!/bin/sh
set -e

flash_fastboot()
{
	fastboot flash BOOT arch/arm/boot/boot.img
	sleep 1
	fastboot flash RECOVERY arch/arm/boot/boot.img
	sleep 1
	fastboot reboot
	echo "OK"
}

flash_heimdall()
{
	heimdall flash \
		--BOOT arch/arm/boot/boot.img \
		--RECOVERY arch/arm/boot/boot.img
	echo "OK"
}

install_parabola_no_modules()
{
  host="$1"

  rsync arch/arm/boot/zImage "${host}:/boot/vmlinuz-linux-custom"
  rsync arch/arm/boot/dts/*.dtb "${host}:/boot/dtbs/linux-custom/"
  rsync -a tests "${host}:"

  ssh "${host}" "test -f /boot/initramfs-linux-custom.img || mkinitcpio -p linux-custom"
}

usage()
{
	echo "Usage: $0 <fastboot|heimdall>"
	echo "Usage: $0 parabola <host>"
	exit 1
}

if [ $# -eq 1 -a "$1" = "fastboot" ] ; then
	flash_fastboot
elif [ $# -eq 1 -a "$1" = "heimdall" ] ; then
	flash_heimdall
elif [ $# -eq 2 -a "$1" = "parabola" ] ; then
	install_parabola_no_modules "$2"
else
	usage
fi
