#!/bin/bash
# Inject -fno-emulated-tls into the GENERATED mesa build.ninja (idempotent).
# Why: OHOS clang defaults to -femulated-tls for musl; mesa's emutls address
# table is private per __emutls_get_address copy, so libglapi writes its copy
# while libGLESv*_impl entry stubs read an empty one -> glGetString() always
# NULL. The flag must land on BOTH compile rules (c_COMPILER / cpp_COMPILER)
# AFTER meson configure has produced build-ohos/ and BEFORE ninja compiles.
# Usage: bash apply-emutls-flag.sh <mesa3d_dir>   (dir containing build-ohos/)
set -e
M=${1:-/root/OpenHarmony/5.1-Release/third_party/mesa3d}
F=$M/build-ohos/build.ninja
[ -f "$F" ] || { echo "no build.ninja under $M/build-ohos (run configure first)"; exit 1; }
grep -q -- "-fno-emulated-tls" "$F" && { echo "already injected: $F"; exit 0; }

python3 - "$F" <<'EOF'
import sys
p = sys.argv[1]
lines = open(p).read().split('\n')
out, inrule = [], False
for ln in lines:
    if ln.startswith('rule '):
        inrule = ln.strip() in ('rule c_COMPILER', 'rule cpp_COMPILER')
    elif inrule and ln.lstrip().startswith('command =') and '-fno-emulated-tls' not in ln:
        ln = ln.replace(' -MD -MQ', ' -fno-emulated-tls -MD -MQ', 1)
    out.append(ln)
open(p, 'w').write('\n'.join(out))
EOF
echo "injected into $(grep -c -- '-fno-emulated-tls' "$F") rule line(s): $F"
