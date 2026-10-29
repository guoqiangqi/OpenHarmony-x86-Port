# OpenHarmony x86_general QEMU 移植仓库

在 QEMU 上构建、启动、验证的 OpenHarmony x86 镜像移植工程，覆盖两个版本：

| 目录 | 版本 | 状态 |
|------|------|------|
| [`ohos-5.1/`](ohos-5.1/) | OpenHarmony 5.1.0 Release（API 14） | 已交付：镜像启动 + 截图验证通过（全像素 65,000+ 色锁屏 UI） |
| [`ohos-6.1/`](ohos-6.1/) | OpenHarmony 6.1 Release（API 23） | 已交付：镜像启动 + 截图验证通过（66,027 色），见其 README §8–§10 |

两个版本的图形策略相反（5.1 用 marker 走 CPU 渲染，6.1 必须 GL 全通），
拼盘与验证步骤互不通用，请分别按各自 README 操作。

## 仓库结构

```
├── ohos-5.1/         5.1 移植：patches/（6 个 git patch + emutls 脚本 + 新文件）
│                     scripts/disk_assemble.sh
│                     docs/（使用说明 / 根因分析 / 构建手册 / 排查记录）
├── ohos-6.1/         6.1 移植：patches/（opc-trio + ohos-repos + untracked 三组）
│                     scripts/（apply-x86-port-patches / setup-sdk / disk_assemble / fixup-disk）
├── common/           launch_qemu.sh / verify_snapshot.sh（两版通用；verify 支持
│                     --sdk18/--sdk23 选择匹配的 hdc）
└── tools/            drmprobe、eglprobe/wrapprobe（显示与 EGL 探针）、
                      mesa-libs/（-fno-emulated-tls 修复版 mesa 库，构建法见 5.1 补丁 05）
```

## 上游源码管理策略

OpenHarmony 本体（500+ git 仓）不适用 git submodule，用 repo 工具按 tag 同步：

```bash
repo init -u https://gitee.com/openharmony/manifest.git -b refs/tags/OpenHarmony-v<N>-Release
repo sync -c -j8 && repo forall -c "git lfs pull"
```

opc 三仓（x86 硬件层，gitee.com/diemit）是小仓，版本已固定到精确 commit
（见各 README 的基线表），可直接 clone checkout，或自行改成 submodule。

本仓库只包含移植增量（补丁、脚本、文档、探针）与 mesa 重编产物
（`tools/mesa-libs/`，约 58M），不含任何 OpenHarmony 源码。

## 快速上手

```bash
# 5.1
cd ohos-5.1 && cat README.md        # 构建 → 拼盘 → 启动 → 截图验证
# 6.1
cd ohos-6.1 && cat README.md        # 同上；注意拼盘后必须执行 fixup-disk.sh
```

QEMU 环境要求见 `common/launch_qemu.sh` 头注释（OVMF、virtio-gpu-pci、
e1000 slirp 端口映射 5555 → 55555）。
