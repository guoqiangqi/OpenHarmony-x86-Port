# OpenHarmony x86_general QEMU 移植仓库

可在 QEMU 上构建、启动、验证的 OpenHarmony x86 镜像移植工程，覆盖两个版本：

| 分支 | 版本 | 状态 |
|------|------|------|
| [`ohos-5.1/`](ohos-5.1/) | OpenHarmony 5.1.0 Release（API 14） | ✅ 完整交付：镜像启动 + 截图非黑（65k 色）端到端验证 |
| [`ohos-6.1/`](ohos-6.1/) | OpenHarmony 6.1 Release（API 23） | 🚧 镜像可构建、可启动到用户态；剩余内核 slub BUG 等（见其 README §8） |

## 仓库结构

```
├── ohos-5.1/         5.1 移植：patches/（5 个 git patch + emutls 脚本 + 新文件）
│   │                 scripts/disk_assemble.sh   docs/（用法/根因/排查史）
├── ohos-6.1/         6.1 移植：patches/（opc-trio + ohos-repos + untracked 三组）
│   │                 scripts/（apply-x86-port-patches / setup-sdk / disk_assemble）
├── common/           launch_qemu.sh / verify_snapshot.sh（两版本通用）
└── tools/            drmprobe（raw ioctl DRM 探针）、wrapprobe/eglprobe（EGL/GL 探针）、
                      mesa-libs/（-fno-emulated-tls 修复版 mesa 10 库，构建法见 5.1/patches/05）
```

## 上游源码管理策略

**OpenHarmony 本体（500+ git 仓）不适用 git submodule**，用 repo 工具按 tag 同步：

```bash
repo init -u https://gitee.com/openharmony/manifest.git -b refs/tags/OpenHarmony-v<N>-Release
repo sync -c -j8 && repo forall -c "git lfs pull"
```

**opc 三仓（x86 硬件层）**是 gitee.com/diemit 下的三个小仓，版本树已 pin 到精确
commit（见各 README §1.1），可直接 clone checkout，或自行改成 submodule。

本仓库**只含移植增量**（补丁/脚本/文档/探针），不含任何 OpenHarmony 源码；
除 `tools/mesa-libs/`（mesa 重编产物，58M，产线见 ohos-5.1/patches/05+apply-emutls-flag.sh）
外无二进制。

## 快速上手

```bash
# 5.1（成品路线）
cd ohos-5.1 && cat README.md        # 构建→拼盘→启动→验证 snapshot 非黑
# 6.1（进行中）
cd ohos-6.1 && cat README.md        # 移植全记录 + 复现步骤 + 剩余工作
```

QEMU 环境要求见 `common/launch_qemu.sh` 头注释（OVMF、virtio-gpu-pci、e1000 slirp 5555→55555）。
