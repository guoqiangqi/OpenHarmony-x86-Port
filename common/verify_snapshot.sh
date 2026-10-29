#!/bin/bash
# Take a device screenshot and verify it is not black (>=60000 unique colors,
# full-pixel count). Works for both the 5.1 and 6.1 images: the hdc client
# must match the guest hdcd (5.1 -> SDK linux/18, 6.1 -> SDK linux/23).
# Usage: verify_snapshot.sh [out.jpeg] [--sdk18|--sdk23]
set -e
HDC18=/root/OpenHarmony/5.1-Release/prebuilts/ohos-sdk/linux/18/toolchains/hdc
HDC23=/root/OpenHarmony/6.1-Release/prebuilts/ohos-sdk/linux/23/toolchains/hdc
HDC=$HDC18
PORT=5555
OUT=${1:-/tmp/verify_snapshot.jpeg}
[ "$2" = "--sdk23" ] && HDC=$HDC23
[ "$2" = "--sdk18" ] && HDC=$HDC18

# If hdc was previously started for the other SDK version, restart the daemon
# for this one (two hdc generations cannot share the same daemon).
if ! timeout 10 $HDC list targets 2>/dev/null | grep -q "127.0.0.1:$PORT"; then
    timeout 10 $HDC kill >/dev/null 2>&1 || true
    timeout 10 $HDC start >/dev/null 2>&1 || true
fi
timeout 10 $HDC tconn 127.0.0.1:$PORT >/dev/null 2>&1 || true
$HDC shell "snapshot_display -f /data/local/tmp/verify.jpeg" | grep -q success
$HDC file recv /data/local/tmp/verify.jpeg "$OUT" >/dev/null
python3 - "$OUT" <<'EOF'
import sys
from PIL import Image
import numpy as np
im = Image.open(sys.argv[1]).convert('RGB')
a = np.asarray(im)
n = len(im.getcolors(maxcolors=2000000))
mean = a.mean(axis=(0,1)).round(1)
print(f"colors={n} mean={mean}")
if n >= 60000:
    print("PASS: screenshot has content (lockscreen UI)")
else:
    print("FAIL: screenshot looks black/empty -- see docs (cold boot + screen timeout formula)")
    sys.exit(1)
EOF
