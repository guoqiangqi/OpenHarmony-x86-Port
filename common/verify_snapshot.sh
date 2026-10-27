#!/bin/bash
# Take a device screenshot and verify it is not black (>=60000 unique colors).
# Usage: verify_snapshot.sh [out.jpeg]
set -e
HDC=/root/OpenHarmony/5.1-Release/prebuilts/ohos-sdk/linux/18/toolchains/hdc
OUT=${1:-/tmp/verify_snapshot.jpeg}
timeout 10 $HDC tconn 127.0.0.1:5555 >/dev/null 2>&1 || true
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
    print("FAIL: screenshot looks black/empty -- see docs (cold boot + timeout formula)")
    sys.exit(1)
EOF
