#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	# Add in some more obvious echo comments to track and debug
	echo "********* Cloning Linux Repository *********"
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    echo "********* Building Kernal *********"
    # we'll use the O var to point to our output directory for kbuild
    # Using mrproper to "deep clean" (Removes all gen'd files + config +various backup files)
    make O="${OUTDIR}" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    # Run with defconfig to configure our "virt" arm dev board we'll sim in QEMU
    make O="${OUTDIR}" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    # now run the build using multiple jobs to try and speed things up -j"${nproc}" to match cores
    make -j$(nproc) O="${OUTDIR}" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all

    # check if we have our kernal image, if not exit with a 1 status (we can't move on without an image)
    if [ ! -e "${OUTDIR}/arch/${ARCH}/boot/Image" ]; then
	echo "Kernal build failure: Image not found in build location."
	echo "Check for artifacts at: ${OUTDIR}/arch/${ARCH}/boot/Image"
	exit 1
    fi
    echo "********* Moving Kernal Image *********"
    # We have a valid image now lets move it to the ${OUTDIR}/image like required
    mv "${OUTDIR}/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"
    # remove un-needed build artifacts (deletes from arch/ folder down)
    rm -rf "${OUTDIR}/arch"
    echo "Kernal image build complete!"
fi

echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
echo "********* Creating rootfs *********"
# create a basic set of folders for the rootfs
echo "Creating rootfs and changing to dir"
mkdir -p ${OUTDIR}/rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr,var}
mkdir -p ${OUTDIR}/rootfs/usr/{bin,lib,sbin}
mkdir -p ${OUTDIR}/rootfs/var/{log}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
else
    cd busybox
fi

# TODO: Make and install busybox
echo "********* Building busybox *********"
echo "cleaning busybox"
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} distclean
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX=${OUTDIR}/rootfs install

# According to comments during last run I may need busybox to have setuid root
#chmod u+s ${OUTDIR}/rootfs/bin/busybox

echo "********* Checking Library dependencies *********"
cd ${OUTDIR}/rootfs
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
echo "********* Setting up SYSROOT and Library dependencies *********"
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

cp -a ${SYSROOT}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib
cp -a ${SYSROOT}/lib64/libm.so.* ${OUTDIR}/rootfs/lib64
cp -a ${SYSROOT}/lib64/libresolv.so.* ${OUTDIR}/rootfs/lib64
cp -a ${SYSROOT}/lib64/libc.so.* ${OUTDIR}/rootfs/lib64

# TODO: Make device nodes
echo "********* Make Device Nodes *********"
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1

# TODO: Clean and build the writer utility
echo "********* Build writer util *********"
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
echo "********* Copy finder and related exec to target rootfs /home *********"
# change to finder app dir, this lets us copy recursively
cd ${FINDER_APP_DIR}
cp -r * ${OUTDIR}/rootfs/home
# we need to back up one dir to get the conf folder
cp -r ../conf ${OUTDIR}/rootfs

# TODO: Chown the root directory
echo "********* Chown root directory *********"
cd ${OUTDIR}/rootfs
sudo chown -R root:root *
# TODO: Create initramfs.cpio.gz
echo "********* Create initramfs.cpio.gz *********"
find . | cpio -H newc -o | gzip -9 > ${OUTDIR}/initramfs.cpio.gz
