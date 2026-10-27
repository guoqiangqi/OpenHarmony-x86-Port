#!/bin/bash
# Launch the delivery disk under QEMU and leave the monitor socket ready.
# Usage: launch_qemu.sh <disk.img> [workdir]
set -e
IMG=${1:-/root/ohos-x86-delivery/images/ohos_disk_20261020.img}
W=${2:-/tmp/ohos-run}
mkdir -p "$W"
[ -f /root/OVMF_VARS.fd ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd /root/OVMF_VARS.fd

setsid nohup qemu-system-x86_64 \
  -machine pc -cpu max -m 8192 -smp 8 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/root/OVMF_VARS.fd \
  -drive file="$IMG",format=raw,if=ide \
  -device virtio-gpu-pci \
  -netdev user,id=n1,hostfwd=tcp::5555-:55555 -device e1000,netdev=n1 \
  -display none -serial file:"$W/serial.log" \
  -monitor unix:"$W/qemu-mon.sock",server,nowait \
  -no-reboot > "$W/qemu.log" 2>&1 < /dev/null &
echo "QEMU started (log/serial/monitor under $W). Waiting for hdc..."
HDC=/root/OpenHarmony/5.1-Release/prebuilts/ohos-sdk/linux/18/toolchains/hdc
for i in $(seq 1 90); do
  sleep 4
  R=$(timeout 6 $HDC tconn 127.0.0.1:5555 2>/dev/null | tail -1)
  if echo "$R" | grep -q connected; then echo "hdc connected (loop $i)"; break; fi
done
$HDC shell "power-shell timeout -o 3600000" >/dev/null 2>&1 || true
echo "Screen-off timeout extended. Wait ~3.5 min for SystemUI, then run verify_snapshot.sh"
