#!/bin/bash
# Assemble a bootable GPT disk for QEMU from OpenHarmony 6.1 (x86_general) images.
# system.img is 4GiB; partition layout: ESP 512M | system 4096M | vendor 512M | data 1536M
set -e
IMG=${1:-./ohos_disk_61.img}
P=$(cd "$(dirname "$0")" && pwd)
[ -f "$IMG" ] && { echo "exists, skip: $IMG"; exit 0; }

truncate -s 6700M "$IMG"
sgdisk -o "$IMG"
sgdisk -n 1:0:+512M  -t 1:ef00 -c 1:"ESP"    "$IMG"
sgdisk -n 2:0:+4096M -t 2:8300 -c 2:"system" "$IMG"
sgdisk -n 3:0:+512M  -t 3:8300 -c 3:"vendor" "$IMG"
sgdisk -n 4:0:0      -t 4:8300 -c 4:"data"   "$IMG"

echo "dd partition images..."
dd if=$P/system.img   of="$IMG" bs=4M conv=sparse,notrunc iflag=skip_bytes oflag=seek_bytes,count_bytes skip=0 seek=$(((1+512)*1024*1024))         count=$((4096*1024*1024)) status=none
dd if=$P/vendor.img   of="$IMG" bs=4M conv=sparse,notrunc iflag=skip_bytes oflag=seek_bytes,count_bytes skip=0 seek=$(((1+512+4096)*1024*1024))     count=$((512*1024*1024))  status=none
dd if=$P/userdata.img of="$IMG" bs=4M conv=sparse,notrunc iflag=skip_bytes oflag=seek_bytes,count_bytes skip=0 seek=$(((1+512+4096+512)*1024*1024)) count=$((1536*1024*1024)) status=none

# ESP: vfat with grub + kernel + ramdisk (root=PARTLABEL layout matches the 5.1 proven disk)
LOOP=$(losetup -f --show -P "$IMG")
trap 'umount -l ${LOOP}p1 2>/dev/null; losetup -d $LOOP' EXIT
mkfs.vfat -n ESP ${LOOP}p1
M=/mnt/esp_asm61 && mkdir -p $M && mount ${LOOP}p1 $M
mkdir -p $M/EFI/BOOT
cat > /tmp/grub61.cfg <<'EOF'
set timeout=0
set default=0
set root=(hd0,gpt1)
serial --unit=0 --speed=115200
terminal_output serial console
menuentry "OpenHarmony 6.1 x86_general" {
    linux /bzImage root=PARTLABEL=system rootfstype=ext4 rootwait rw hardware=x86_general console=ttyS0,115200 console=tty0
}
EOF
cp /tmp/grub61.cfg $M/grub.cfg
cp $P/bzImage $P/ramdisk.img $M/
grub-mkstandalone -O x86_64-efi --modules="part_gpt part_msdos fat ext2 search" -o $M/EFI/BOOT/BOOTX64.EFI "boot/grub/grub.cfg=/tmp/grub61.cfg"
umount $M
echo "OK: $IMG"
sgdisk -p "$IMG"
