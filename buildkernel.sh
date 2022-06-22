SRC=/media/system/root/linux
KERNEL_OUT=$(realpath $SRC/../KERNEL_OBJ)
PACKAGE_DIR=$(realpath $SRC/../KERNEL_PACKAGE)
GCC=arm-none-eabi-
DTB="exynos4412-i9300.dtb"
ZIP_PACKAGE=$SRC/PACKAGING/pmos-samsung-m0-linux-kernel.zip

# Copy prerequisite files from usr directory
test -f $KERNEL_OUT/usr/busybox.static || cp -r $SRC/PACKAGING/usr $KERNEL_OUT

# Don't append tags to the kernel version
touch $SRC/.scmversion

make -j8  CFLAGS_MODULE="-fno-pic" -C $SRC O=$KERNEL_OUT ARCH=arm CROSS_COMPILE="$GCC" postmarketos_exynos4_defconfig
make -j8 -k  CFLAGS_MODULE="-fno-pic" -C $SRC O=$KERNEL_OUT ARCH=arm CROSS_COMPILE="$GCC" zImage

# Build DTB and concat with the kernel
make -C $SRC O=$KERNEL_OUT ARCH=arm CROSS_COMPILE="$GCC" $DTB
cat $KERNEL_OUT/arch/arm/boot/zImage $KERNEL_OUT/arch/arm/boot/dts/${DTB} > $KERNEL_OUT/arch/arm/boot/zImage-dtb

# Clean up any modules leftover
rm -r $PACKAGE_DIR/*
mkdir -p $PACKAGE_DIR/boot

# Build and install modules
make -j8 -C $SRC O=$KERNEL_OUT ARCH=arm CROSS_COMPILE="$GCC" modules
make -j8 -C $SRC O=$KERNEL_OUT ARCH=arm CROSS_COMPILE="$GCC" INSTALL_MOD_PATH=$PACKAGE_DIR modules_install
find $PACKAGE_DIR/lib -name "*.ko" -exec "$GCC"strip --strip-unneeded {} \;

# Build boot.img
mkbootimg  \
  --kernel $KERNEL_OUT/arch/arm/boot/zImage-dtb \
  --ramdisk $SRC/PACKAGING/boot.img-ramdisk.gz \
  --base 0x40000000 \
  --pagesize 2048 \
  --output $KERNEL_OUT/boot.img

size=$(for i in $KERNEL_OUT/boot.img; do stat --format "%s" "$i" | tr -d '\n'; echo +; done; echo 0);
total=$(( $( echo "$size" ) ));
printname=$(echo -n "$KERNEL_OUT/boot.img" | tr " " +);
img_blocksize=4224;
twoblocks=$((img_blocksize * 2)); onepct=$(((((8650752 / 100) - 1) / img_blocksize + 1) * img_blocksize));
reserve=$((twoblocks > onepct ? twoblocks : onepct));
maxsize=$((8650752 - reserve));

echo "$printname maxsize=$maxsize blocksize=$img_blocksize total=$total reserve=$reserve";

if [ "$total" -gt "$maxsize" ]; then
  echo "error: $printname too large ($total > [8650752 - $reserve])"; false;
elif [ "$total" -gt $((maxsize - 32768)) ]; then
  echo "WARNING: $printname approaching size limit ($total now; limit $maxsize)";
fi

# Build flashable ZIP package
cp $KERNEL_OUT/boot.img $PACKAGE_DIR/boot
tar -czvf $PACKAGE_DIR/rootfs.tar.gz -C $PACKAGE_DIR boot lib

cp $ZIP_PACKAGE $PACKAGE_DIR
cd $PACKAGE_DIR
ZIP=$(basename $ZIP_PACKAGE)
zip -9r $ZIP rootfs.tar.gz

echo $(realpath $ZIP)
