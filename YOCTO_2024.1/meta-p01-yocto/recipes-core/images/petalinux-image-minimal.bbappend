# P01 image content: the packages PetaLinux selects in project-spec/configs/rootfs_config on top
# of petalinux-image-minimal and the Enclustra refdes additions.
IMAGE_INSTALL:append = " \
    e2fsprogs-mke2fs \
    dosfstools \
    p01-app \
    packagegroup-petalinux-vitisai \
"

# No SD card image (wic, added by meta-enclustra-refdes for enclustra-sd; wic.qemu-sd, added by
# zynqmp-generic for QEMU): the boot files and the ext4 rootfs are copied to the SD card by hand, as
# in the PetaLinux flow. No JFFS2 image either (raw flash file system; the rootfs is on the SD card).
# Result: tar.gz cpio cpio.gz cpio.gz.u-boot ext4, the PetaLinux image types without jffs2.
IMAGE_FSTYPES:remove = "wic wic.gz wic.bmap wic.qemu-sd jffs2"

# Users: petalinux sets its password at the first login and may use sudo; root stays locked.
inherit extrausers plnx-useradd-sudoers
EXTRA_USERS_PARAMS = " \
    groupadd -r aie; \
    useradd -p '' petalinux; \
    passwd-expire petalinux; \
    usermod -a -G audio,video,aie,input petalinux; \
"
EXTRA_USERS_SUDOERS = "petalinux ALL=(ALL) ALL;"
