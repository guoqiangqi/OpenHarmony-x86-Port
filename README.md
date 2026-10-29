# OpenHarmony x86_general QEMU 移植仓库

本仓库收集了把 OpenHarmony x86_general 产品移植到 QEMU 虚拟机的全部工作成果，
包括补丁、组装脚本、验证工具和文档，覆盖两个版本：

| 目录 | 版本 | 状态 |
|------|------|------|
| [`ohos-5.1/`](ohos-5.1/) | OpenHarmony 5.1.0 Release（API 14） | 已交付：系统可启动，截图验证通过（锁屏界面全像素统计 65,000+ 色） |
| [`ohos-6.1/`](ohos-6.1/) | OpenHarmony 6.1 Release（API 23） | 已交付：系统可启动，截图验证通过（66,027 色），详见其 README 第 8–10 节 |

两个版本采用的图形策略正好相反：5.1 依靠一个标记文件让全系统回退到 CPU 渲染，
6.1 则要求 OpenGL 完整初始化。因此磁盘组装和验证步骤互不通用，请分别按各自的
README 操作。

## 仓库结构

```
├── ohos-5.1/         5.1 移植：patches/（6 个 git 补丁 + emutls 脚本 + 新增文件）
│                     scripts/disk_assemble.sh
│                     docs/（使用说明 / 根因分析 / 构建手册 / 排查记录）
├── ohos-6.1/         6.1 移植：patches/（opc-trio + ohos-repos + untracked 三组）
│                     scripts/（apply-x86-port-patches / setup-sdk / disk_assemble / fixup-disk）
├── common/           launch_qemu.sh / verify_snapshot.sh（两个版本通用；
│                     verify_snapshot.sh 支持 --sdk18/--sdk23 选择对应版本的 hdc）
└── tools/            drmprobe、eglprobe/wrapprobe（显示与 EGL 探针）、
                      mesa-libs/（-fno-emulated-tls 修复版 mesa 库，构建方法见 5.1 补丁 05）
```

## 上游源码的管理方式

OpenHarmony 主干由 500 多个 git 仓库组成，不适合用 submodule 管理，按社区惯例
用 repo 工具按 tag 同步：

```bash
repo init -u https://gitee.com/openharmony/manifest.git -b refs/tags/OpenHarmony-v<N>-Release
repo sync -c -j8 && repo forall -c "git lfs pull"
```

x86 硬件适配层的 opc 三个仓库（gitee.com/diemit）体量很小，版本已固定到具体
commit（见各 README 的基线表），直接 clone 后 checkout 即可，也可以自行改成
submodule。

本仓库只包含移植产生的增量（补丁、脚本、文档、探针）和重新编译的 mesa 库
（`tools/mesa-libs/`，约 58M），不含任何 OpenHarmony 源码。

## 快速上手

```bash
# 5.1
cd ohos-5.1 && cat README.md        # 构建、组装磁盘、启动、截图验证
# 6.1
cd ohos-6.1 && cat README.md        # 流程相同；组装磁盘后必须再执行 fixup-disk.sh
```

QEMU 环境要求见 `common/launch_qemu.sh` 的头部注释（OVMF 固件、virtio-gpu-pci
显卡、e1000 网卡经 slirp 做端口映射 5555 → 55555）。
