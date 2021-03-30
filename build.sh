export CROSS_COMPILE=/media/disk/root/REPL_LOS18/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androidkernel-
export ARCH=arm
#make midas_defconfig O=../out_modem2
make replicant_defconfig O=../out
make -j8 O=../out
