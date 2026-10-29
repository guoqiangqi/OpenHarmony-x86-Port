#!/bin/bash
# Post-assembly fixups for the OpenHarmony 6.1 x86_general QEMU disk.
# The stock images from the build need these before the system actually works:
#
#  1. VENDOR MERGE: 6.1 splits chipset content into vendor.img, but init reads
#     /vendor/etc/fstab.x86_general BEFORE any partition is mounted (the 5.1
#     runtime model embeds the vendor tree inside the system image root).
#     Without the merge nothing mounts: /vendor stays an empty dir, every HDF
#     host (composer/allocator/audio/input...) fails to start ("pathArgs
#     invalid"), multimodalinput freezes on the composer HDI Get, and the
#     fstab failure leaves the root fs read-only.
#  2. DATA REFORMAT: the built userdata.img is f2fs but CONFIG_F2FS_FS=m and
#     f2fs.ko is not built/shipped, and fstab.x86_general says ext4. Reformat
#     the data partition as ext4 (the system provisions its /data skeleton).
#  3. GL ENABLEMENT: render_service has no CPU fallback on 6.1 — GL must work.
#     graphic.cfg env (LIBGL_ALWAYS_SOFTWARE/LIBGL_DRIVERS_PATH) + kms_swrast/
#     swrast/virtio_gpu dri aliases in /system/lib64.
#  4. hdf_devhost: copy the drivers_adapter-built /vendor/bin/hdf_devhost
#     (the component is missing from the stock product config; it is added by
#     the opc-trio vendor patch, but assembled-from-stock-images disks don't
#     have it — take it from the tree out dir).
#
# Usage: bash fixup-disk.sh <disk.img> <tree-out-dir=x86_general> [--marker]
#   --marker: also drop /data/egl_no_init (5.1-style forced CPU rendering; NOT
#   recommended on 6.1 — RS 5.1-style CPU fallback does not exist, verified
#   GL init "ver 1.4" is the working path)
set -e
IMG=$(realpath "$1"); OUT=$(realpath "$2"); MARKER=${3:-}

losetup -fP "$IMG"; sleep 2
D=$(losetup -j "$IMG" | head -1 | cut -d: -f1)
trap 'umount /mnt/fx-v /mnt/fx-s /mnt/fx-d 2>/dev/null; losetup -d "$D" 2>/dev/null' EXIT
mkdir -p /mnt/fx-v /mnt/fx-s /mnt/fx-d
mount "${D}p3" /mnt/fx-v; mount "${D}p2" /mnt/fx-s

echo "== [1/4] merge vendor tree into system root /vendor =="
cp -a /mnt/fx-v/. /mnt/fx-s/vendor/
[ -f "$OUT/hdf/drivers_adapter/hdf_devhost" ] && \
    cp -v "$OUT/hdf/drivers_adapter/hdf_devhost" /mnt/fx-s/vendor/bin/
ls /mnt/fx-s/vendor/etc/fstab.x86_general >/dev/null || { echo "fstab missing after merge!"; exit 1; }

echo "== [2/4] reformat data partition as ext4 =="
umount /mnt/fx-d 2>/dev/null || true
mkfs.ext4 -F -L data "${D}p4" >/dev/null 2>&1
mount "${D}p4" /mnt/fx-d
mkdir -p /mnt/fx-d/local/tmp

echo "== [3/4] GL enablement (graphic.cfg global env + dri aliases) =="
# The env must reach EVERY EGL client process (render_service AND apps:
# SystemUI/bootanimation run in appspawn sandboxes and cannot find the
# kms_swrast DRI driver without LIBGL_DRIVERS_PATH — their eglInitialize
# fails and ArkUI renders nothing). init's "export" propagates to all
# services it spawns, so put it in the init job instead of a single
# service's env block.
python3 - <<'EOF'
import json
p='/mnt/fx-s/system/etc/init/graphic.cfg'
c=json.load(open(p))
for job in c['jobs']:
    if job['name']=='init':
        for a in ["export LIBGL_ALWAYS_SOFTWARE 1","export LIBGL_DRIVERS_PATH /system/lib64"]:
            if a not in job['cmds']:
                job['cmds'].append(a)
json.dump(c,open(p,'w'),indent=4,ensure_ascii=False)
EOF
cd /mnt/fx-s/system/lib64
for a in kms_swrast_dri.so swrast_dri.so virtio_gpu_dri.so; do ln -sf libgallium_dri.so $a; done
cd /

echo "== [4/4] optional marker =="
if [ "$MARKER" = "--marker" ]; then touch /mnt/fx-d/egl_no_init; echo "egl_no_init placed"; fi

sync; echo "disk ready: $IMG"
