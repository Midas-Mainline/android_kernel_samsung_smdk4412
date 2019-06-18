#!/bin/bash
# Copyright (C) 2019 Denis 'GNUtoo' Carikli <GNUtoo@cyberdimension.org>

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

set -e

supported_devices=" \
  i9300 \
  i9305 \
  n710x \
"

supported_os=" \
  Android \
  GNU/Linux \
"

supported_bootloaders="\
  s-boot \
  u-boot \
"

sentinel=""
dtb=""
cmdline=""

print_supported()
{
  printf "<"
  for elm in $@ ; do
    printf "${elm}|"
  done
  printf "\b> "
}

# Example:
# check_supported i9300 device ${supported_devices}
check_supported()
{
  elm="$1"
  type="$2"
  shift 2

  for supported in $@ ; do
    if [ "${elm}" = "${supported}" ] ; then
      return 0
    fi
  done

  echo "Invalid ${type} '${elm}'. Usage:"
  usage
  exit 1
}

usage()
{
  printf "%s " "$0"
  print_supported ${supported_devices}
  print_supported ${supported_bootloaders}
  print_supported ${supported_os}
  printf "\b\n"

  exit 1
}

add_secure_firmware_node()
{
  dtsi="arch/arm/boot/dts/exynos4412-midas.dtsi"
  if ! grep "firmware@204f000" ${dtsi} 2>&1 > /dev/null ; then
      # Add the node
      cat ${dtsi} | \
      awk 'BEGIN {chosen_found=0} \
        { \
          {print $0}
          if ($0 == "\tchosen {") {chosen_found=1}; fi; \
          if (chosen_found == 1 && $0 == "\t};") { \
            chosen_found=0; \
            print(""); \
            print("\tfirmware@204f000 {"); \
            print("\t\tcompatible = \"samsung,secure-firmware\";"); \
            print("\t\treg = <0x0204F000 0x1000>;"); \
            print("\t};"); \
          }; fi; \
        }' \
      > ${dtsi}.1
      mv -f ${dtsi}.1 ${dtsi}
    fi
}

remove_secure_firmware_node()
{
    dtsi="arch/arm/boot/dts/exynos4412-midas.dtsi"
    cat ${dtsi}  | \
    awk 'BEGIN {do_print=1} \
	{ \
	  if ($0 == "\tfirmware@204f000 {") {do_print=0}; fi; \
	  if (do_print == 1) {print $0} ; fi; \
	  if ($0 == "\t};") {do_print=1}; fi; \
	}' \
	> ${dtsi}.1
   mv -f ${dtsi}.1 ${dtsi}
}

add_arm_decompressor_tlb_flush()
{
  head_s_file="arch/arm/boot/compressed/head.S"

  if ! grep -A 1 -P '^\t\tmcrne\tp15, 0, r0, c8, c7, 0\t@ flush I,D TLBs$' \
    arch/arm/boot/compressed/head.S | \
    tail -n1 | \
    grep -P '^\t\tmcr\tp15, 0, r0, c7, c5, 4\t@ ISB$' \
    2>&1 > /dev/null ; then
    cat ${head_s_file} | \
    awk 'BEGIN {armv7_mmu_cache_on_found=0;} \
         { \
           if ($0 == "__armv7_mmu_cache_on:") { \
             armv7_mmu_cache_on_found=1; \
           }; fi; \
           if (armv7_mmu_cache_on_found == 1 && \
              ($0 == "\t\tmcrne\tp15, 0, r3, c2, c0, 0\t@ load page table pointer")) { \
               armv7_mmu_cache_on_found=0; \
               print("\t\tmcrne\tp15, 0, r0, c8, c7, 0\t@ flush I,D TLBs"); \
               print("\t\tmcr\tp15, 0, r0, c7, c5, 4\t@ ISB"); \
            }; fi; \
           {print $0} \
         }' \
    > ${head_s_file}.1
    mv -f ${head_s_file}.1 ${head_s_file}
fi
}

remove_arm_decompressor_tlb_flush()
{
    # I'm unlikely to make other changes to that file
    git checkout -- arch/arm/boot/compressed/head.S
}

check_kconfig_empty_config()
{
    config="$1"

    if [ "${config}" = "" ] ; then
	echo "Exiting: empty kconfig configuration"
	exit 1
    fi
}

set_kconfig_value()
{
    config="$1"
    check_kconfig_empty_config "${config}"

    shift 1
    value="$@"

    sed "/^${config}=\"*\"/d" -i .config
    echo "${config}=\"${value}\"" >> .config
}

set_kconfig_y()
{
    config="$1"

    check_kconfig_empty_config "${config}"
    sed "/^${config}=*/d" -i .config
    echo "${config}=y" >> .config
}

unset_kconfig()
{
    config="$1"

    check_kconfig_empty_config "${config}"
    sed "/^${config}=*/d" -i .config
    echo "# ${config} is not set" >> .config
}

jobs="-j$(grep processor /proc/cpuinfo  | wc -l) -k"

source arm_build.sh

if [ $# -ne 3 ] ; then
    usage
fi

device="$1"
bootloader="$2"
os="$3"

check_supported "${device}" "device" ${supported_devices}
check_supported "${bootloader}" "bootloader" ${supported_bootloaders}
check_supported "${os}" "OS" ${supported_os}

dtb="exynos4412-${device}.dtb"

make replicant_defconfig

if [ "${os}" = "Android" ] ; then
    set_kconfig_y CONFIG_USB_FUNCTIONFS
    cmdline="${cmdline} root=PARTLABEL=SYSTEM init=/init rootwait"
    cmdline="${cmdline} androidboot.hardware=smdk4x12"
    cmdline="${cmdline} androidboot.selinux=permissive enforcing=0"
    cmdline="${cmdline} exynosdrm.pixel_order=1"
elif [ "${os}" = "GNU/Linux" ] ; then
  unset_kconfig CONFIG_USB_FUNCTIONFS
  cmdline="${cmdline} root=/dev/mmcblk0 rw"
fi

cmdline="${cmdline} buildvariant=eng device=${device} rootwait console=ttySAC2,115200 loglevel=8"
cmdline="${cmdline} no_console_suspend"

if [ "${bootloader}" = "s-boot" ] ; then
    # boot.img cmdline is ignored and Android userspace needs access
    # to the IMEI which is passed as boot arguments
    unset_kconfig CONFIG_CMDLINE_FORCE
    unset_kconfig CONFIG_CMDLINE_FROM_BOOTLOADER
    set_kconfig_y CONFIG_CMDLINE_EXTEND
    set_kconfig_value CONFIG_CMDLINE "${cmdline}"

    # Workaround fatally flawed bootloader which has MMU on
    add_arm_decompressor_tlb_flush
    unset_kconfig CONFIG_STACKPROTECTOR_PER_TASK

    # Make sure that samsung,secure-firmware is present
    add_secure_firmware_node
elif [ "${bootloader}" = "u-boot" ] ; then
    # Make sure that the bootargs comes from the boot.img
    unset_kconfig CONFIG_CMDLINE
    unset_kconfig CONFIG_CMDLINE_FORCE
    unset_kconfig CONFIG_CMDLINE_EXTEND
    set_kconfig_y CONFIG_CMDLINE_FROM_BOOTLOADER

    # Remove samsung,secure-firmware as we have no TrustZone OS
    remove_secure_firmware_node

    # Remove workaround for s-boot if present
    remove_arm_decompressor_tlb_flush
fi

# Save the config
yes '' | make oldconfig
make savedefconfig
cp -f defconfig arch/arm/configs/replicant_defconfig

make ${jobs} \
	zImage \
	"${dtb}" \
	${sentinel}

cat \
	arch/arm/boot/zImage \
	"arch/arm/boot/dts/${dtb}" \
	> arch/arm/boot/zImage.dtb

mkbootimg \
	--base 0x10000000 \
	--kernel arch/arm/boot/zImage.dtb \
	--cmdline "${cmdline}" \
	--output arch/arm/boot/boot.img \
	${sentinel}

du -hs arch/arm/boot/boot.img
unbootimg -i arch/arm/boot/boot.img

if [ "${bootloader}" = "s-boot" ] ; then
  echo "The kenrel is ready at arch/arm/boot/boot.img. You can install it with:"
  echo "heimdall flash --BOOT arch/arm/boot/boot.img --RECOVERY arch/arm/boot/boot.img"
else
  echo OK
fi
