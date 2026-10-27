#!/bin/bash
# Assemble prebuilts/ohos-sdk/linux/23 for the OpenHarmony 6.1 x86_general build.
#
# Why: hvigor ArkTS compilation needs systemapi typings the public SDK lacks,
# while hap linking needs the x86_64 native sysroot libs + host (linux) ets
# toolchain binaries. The source-built (arm64-target) SDK alone ships an
# aarch64 es2abc -> "Exec format error". So we mix:
#   ets/js/previewer toolchains  <- public linux SDK (host x86_64 binaries)
#   ets api + kits typings       <- source-built "ohos" variant (systemapi)
#   native (sysroot + llvm + build) <- source-built sdk-native
#
# Usage: bash setup-sdk.sh <OHOS_61_TREE> [public-sdk.tar.gz]
#   public-sdk.tar.gz: ohos-sdk-windows_linux-public.tar.gz downloaded from
#   https://repo.huaweicloud.com/openharmony/os/6.1-Release/ (2.5G)
set -e
R=$(realpath "$1")
TARBALL=${2:-/root/ohos-sdk-61.tar.gz}
SDKDIR=$R/prebuilts/ohos-sdk/linux/23
V=6.1.0.31

[ -f "$TARBALL" ] || { echo "download public SDK first: $TARBALL missing"; exit 1; }

# 1) source-built SDK (from a finished `./build.sh --product-name ohos-sdk`)
SRC=$R/out/sdk/ohos-sdk
[ -d "$SRC/ohos/ets" ] || { echo "run ohos-sdk product build first (out/sdk/ohos-sdk missing)"; exit 1; }

# 2) extract public linux toolchain components
TMP=$(mktemp -d)
tar xzf "$TARBALL" -C "$TMP" linux/ets-linux-x64-$V-Release.zip linux/js-linux-x64-$V-Release.zip linux/previewer-linux-x64-$V-Release.zip linux/toolchains-linux-x64-$V-Release.zip
for z in ets js previewer toolchains; do
    f=$(ls $TMP/linux/${z}-linux-x64-*.zip)
    unzip -q -o "$f" -d $TMP/${z}_x
done

# 3) install: host toolchains + native from source build, systemapi typings overlay
rm -rf "$SDKDIR"; mkdir -p "$SDKDIR"
cp -r $TMP/ets_x/* "$SDKDIR/ets" 2>/dev/null || { mkdir -p "$SDKDIR/ets"; cp -r $TMP/ets_x/* "$SDKDIR/ets/"; }
cp -r $TMP/js_x/* "$SDKDIR/js"
cp -r $TMP/previewer_x/* "$SDKDIR/previewer"
cp -r $TMP/toolchains_x/* "$SDKDIR/toolchains"
mkdir -p "$SDKDIR/native"
cp -r $R/out/sdk/sdk-native/os-irrelevant/* "$SDKDIR/native/"
cp -r $R/out/sdk/sdk-native/os-specific/linux/* "$SDKDIR/native/"
# systemapi typings (source-built "ohos" variant) replace the public api/kits
rm -rf "$SDKDIR/ets/api" "$SDKDIR/ets/kits"
cp -r $SRC/ohos/ets/api "$SDKDIR/ets/api"
cp -r $SRC/ohos/ets/kits "$SDKDIR/ets/kits"
rm -rf "$TMP"
echo "SDK ready at $SDKDIR"
ls "$SDKDIR"
