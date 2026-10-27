#!/bin/bash
# Apply the x86 port patch set to a freshly synced OpenHarmony 6.1 tree
# (after the opc trio has been cloned in place and its own patch.sh applied).
# Usage: bash apply-x86-port-patches.sh <OHOS_61_TREE>
set -e
R=$(realpath "$1")
HERE=$(cd "$(dirname "$0")/.." && pwd)   # ohos-6.1/

echo "== [1/6] opc trio patch.sh (fuzz) =="
export PRODUCT_PATH=vendor/opc/x86_general DEVICE_NAME=x86_general PRODUCT_COMPANY=opc
cd "$R/vendor/opc/x86_general/patches"
for repo in $(cd "$HERE/patches/opc-trio" >/dev/null; echo); do :; done
# opc's own patch.sh applies with plain patch; run it and allow partial failures
patch -p1 --forward -d "$R/arkcompiler/runtime_core" < ./*/arkcompiler/runtime_core/*.patch 2>/dev/null || true
bash patch.sh 2>&1 | grep -E "FAILED|error" | head -20 || true

echo "== [2/6] fix leftover .rej from patch.sh =="
find "$R" -name '*.rej' -not -path '*/out/*' | head -20 || true

echo "== [3/6] musl loader ini MUST stay upstream 6.1 (opc patch rewrites it) =="
git -C "$R/third_party/musl" checkout -- config/ld-musl-namespace-x86_64.ini

echo "== [4/6] apply x86-port patch set =="
N=0
for p in "$HERE"/patches/ohos-repos/*.patch; do
    # patch files are named NN-<repo-path-with-dashes>.patch
    base=$(basename "$p" .patch); rel=${base#*-}; repo=${rel//-//}
    if git -C "$R/$repo" apply --check "$p" 2>/dev/null; then
        git -C "$R/$repo" apply "$p" && N=$((N+1)) && echo "ok  $repo"
    elif git -C "$R/$repo" apply --check -R "$p" 2>/dev/null; then
        echo "skip $repo (already applied)"
    else
        patch -p1 --forward -f -d "$R/$repo" < "$p" >/dev/null 2>&1 && N=$((N+1)) && echo "fuzz $repo" || echo "FAIL $repo — resolve manually"
    fi
done
for p in "$HERE"/patches/opc-trio/*.patch; do
    base=$(basename "$p" .patch); name=${base#??-}
    case $name in
        vendor-config-x86port)      repo=vendor/opc ;;
        board-kernel-modules-cfg)   repo=device/board/opc ;;
        soc-display-hal-elfutils-lzma) repo=device/soc/opc ;;
    esac
    git -C "$R/$repo" apply "$p" 2>/dev/null && echo "ok  $repo" || echo "skip/FAIL $repo"
done

echo "== [5/6] untracked files (modules_gpu/net, defconfig backup) =="
cp -r "$HERE/patches/untracked/board/." "$R/device/board/opc/" 2>/dev/null || true
cp -r "$HERE/patches/untracked/soc/." "$R/device/soc/opc/" 2>/dev/null || true

echo "== [6/6] kernel: replace with the 5.1-built linux-6.6 tree =="
echo "  see ohos-6.1/README.md §kernel — copy from a patched 5.1 tree"
echo "applied $N ohos-repo patches"
