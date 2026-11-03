#!/bin/bash
# Upgrade a 5.1 displayfix disk (or one assembled from the delivered partition
# images) to the fs-verity build, in place. Required for official-chain HAP
# installs (error 9568393 without it).
#
# What it does:
#   1. ESP: replace bzImage with the fs-verity kernel build
#   2. system: replace the 4 kernel modules (ABI changed with the new config)
#   3. data: add the ext4 "verity" feature flag (mkfs.ext4 default omits it;
#      without it FS_IOC_ENABLE_VERITY returns EOPNOTSUPP)
#   4. system: point key_enable.cfg's require_signatures at 0 — the OHOS 5.1
#      userspace verifies signatures itself; the kernel-side builtin
#      enforcement cannot work here because the certificate provisioning
#      interface (CONFIG_SECURITY_CODE_SIGN) is not enabled
#
# Usage: bash enable-fsverity-disk.sh <disk.img> <parts-dir>
#   <disk.img>  target disk (QEMU must be stopped)
#   <parts-dir> directory with the new artifacts:
#               bzImage, modules/{virtio-gpu.ko,virtio_dma_buf.ko,
#               drm_shmem_helper.ko,e1000.ko}
set -e
IMG=$(realpath "$1"); PARTS=$(realpath "$2")

[ -f "$PARTS/bzImage" ] || { echo "missing $PARTS/bzImage"; exit 1; }
for m in virtio-gpu.ko virtio_dma_buf.ko drm_shmem_helper.ko e1000.ko; do
    [ -f "$PARTS/modules/$m" ] || { echo "missing $PARTS/modules/$m"; exit 1; }
done

losetup -fP "$IMG"; sleep 2
D=$(losetup -j "$IMG" | head -1 | cut -d: -f1)
trap 'umount /mnt/fsv-s /mnt/fsv-e 2>/dev/null; losetup -d "$D" 2>/dev/null' EXIT
mkdir -p /mnt/fsv-s /mnt/fsv-e

echo "== [1/4] ESP bzImage =="
mount "${D}p1" /mnt/fsv-e
cp -v "$PARTS/bzImage" /mnt/fsv-e/bzImage
umount /mnt/fsv-e

echo "== [2/4] system modules =="
mount "${D}p2" /mnt/fsv-s
cp -v "$PARTS"/modules/*.ko /mnt/fsv-s/system/lib/modules/

echo "== [3/4] key_enable.cfg enforcement off =="
sed -i 's|write /proc/sys/fs/verity/require_signatures 1|write /proc/sys/fs/verity/require_signatures 0|' \
    /mnt/fsv-s/system/etc/init/key_enable.cfg
grep -n require_signatures /mnt/fsv-s/system/etc/init/key_enable.cfg
sync; umount /mnt/fsv-s

echo "== [4/4] data ext4 verity feature =="
e2fsck -fy "${D}p4" >/dev/null 2>&1 || true
tune2fs -O verity "${D}p4"
dumpe2fs -h "${D}p4" 2>/dev/null | grep -o "verity" | head -1 && echo "verity feature set"
sync
echo "disk ready: $IMG"
