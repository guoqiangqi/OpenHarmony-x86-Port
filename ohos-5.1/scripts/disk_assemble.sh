#!/bin/bash
# Assemble a bootable GPT disk for QEMU from this package's partition images.
# Layout mirrors the proven working disk: ESP 512M | system 2016M | vendor 512M | data 1536M
# NOTE: partition byte offsets = 1MiB GPT + sum of preceding sizes (sgdisk 2048-sector align).
set -e
IMG=${1:-./ohos_disk.img}
P=$(cd "$(dirname "$0")" && pwd)
[ -f "$IMG" ] && { echo "exists, skip: $IMG"; exit 0; }

truncate -s 4700M "$IMG"
sgdisk -o "$IMG"
sgdisk -n 1:0:+512M  -t 1:ef00 -c 1:"ESP"    "$IMG"
sgdisk -n 2:0:+2016M -t 2:8300 -c 2:"system" "$IMG"
sgdisk -n 3:0:+512M  -t 3:8300 -c 3:"vendor" "$IMG"
sgdisk -n 4:0:+1536M -t 4:8300 -c 4:"data"   "$IMG"

echo "dd partition images..."
dd if=$P/system.img   of="$IMG" bs=4M conv=sparse,notrunc iflag=skip_bytes oflag=seek_bytes,count_bytes skip=0 seek=$(((1+512)*1024*1024))         count=$((2016*1024*1024)) status=none
dd if=$P/vendor.img   of="$IMG" bs=4M conv=sparse,notrunc iflag=skip_bytes oflag=seek_bytes,count_bytes skip=0 seek=$(((1+512+2016)*1024*1024))     count=$((512*1024*1024))  status=none
dd if=$P/userdata.img of="$IMG" bs=4M conv=sparse,notrunc iflag=skip_bytes oflag=seek_bytes,count_bytes skip=0 seek=$(((1+512+2016+512)*1024*1024)) count=$((1536*1024*1024)) status=none

# ESP: vfat with grub + kernel + ramdisk
LOOP=$(losetup -f --show -P "$IMG")
trap 'umount -l ${LOOP}p1 2>/dev/null; losetup -d $LOOP' EXIT
mkfs.vfat -n ESP ${LOOP}p1
M=/mnt/esp_asm && mkdir -p $M && mount ${LOOP}p1 $M
mkdir -p $M/EFI/BOOT
cat > /tmp/grub.cfg <<'EOF'
set timeout=0
set default=0
set root=(hd0,gpt1)
serial --unit=0 --speed=115200
terminal_output serial console
menuentry "OpenHarmony x86_general" {
    linux /bzImage root=/dev/sda2 rootfstype=ext4 rootwait rw hardware=x86_general console=ttyS0,115200 console=tty0
}
EOF
cp /tmp/grub.cfg $M/grub.cfg
cp $P/bzImage $P/ramdisk.img $M/
grub-mkstandalone -O x86_64-efi --modules="part_gpt part_msdos fat ext2" -o $M/EFI/BOOT/BOOTX64.EFI "boot/grub/grub.cfg=/tmp/grub.cfg"
umount $M
echo "OK: $IMG"
sgdisk -p "$IMG"
