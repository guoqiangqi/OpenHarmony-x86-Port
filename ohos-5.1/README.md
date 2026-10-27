# OpenHarmony 5.1 x86_general（显示修复版）构建指南

> 状态：**已完整交付**（2026-10-20）。镜像截图非黑实锤（snapshot 65k 色锁屏 UI），
> 成品盘经三次冷启动端到端验证。本目录固化全部补丁与文档供复现。

## 快速开始

1. 源码获取（同 6.1 流程，换 tag）：
   ```bash
   repo init -u https://gitee.com/openharmony/manifest.git \
       -b refs/tags/OpenHarmony-v5.1.0-Release --no-repo-verify && repo sync -c -j8
   ```
   opc 三仓基线（OpenHarmony-5.1.0-Release 分支）：
   vendor_opc `23d66b68` / device_board_opc `fbf60291` / device_soc_opc `32fb2452`
   （均为 diemit gitee 仓库）
2. Ubuntu 24.04 兼容：host 侧 GN 修复见 docs/构建与部署手册.md §C（stdint/macro-redefined 等）。
3. 打补丁（顺序）：
   ```bash
   bash <trio>/vendor/opc/x86_general/patches/patch.sh     # opc 自带
   for p in patches/0{1,2,3,4,5,6}-*.patch; do git -C <对应仓库> apply $p; done
   cp -r patches/untracked/* <树>/device/{board,soc}/opc/...   # 见 patches/README.md
   ```
   mesa 的 `-fno-emulated-tls`：meson configure 后跑 `bash patches/apply-emutls-flag.sh
   <树>/third_party/mesa3d`（幂等）。
4. `./build.sh --product-name x86_general --ccache`（python3.10 venv）
5. 拼盘：`bash scripts/disk_assemble.sh ./disk.img`（分区镜像放脚本同目录）
6. 启动+验证：`bash ../common/launch_qemu.sh ./disk.img` → `bash ../common/verify_snapshot.sh`

## 文档索引

| 文档 | 内容 |
|------|------|
| docs/USAGE.md | 交付关键说明：内嵌修复清单、QEMU 命令、黄金启动公式、红线 |
| docs/根因分析_截图全黑.md | 为什么黑、怎么修的（GL 写入丢失 / iov 页分裂 / fbcon 竞技场） |
| docs/构建与部署手册.md | 部署 / 增量编译 / marker 开关 |
| docs/HANDOFF_截图非全黑任务.md | 完整排查史 |
| patches/README.md | 补丁依赖序、应用命令、untracked 新文件去处 |

## 黄金启动公式（截图非黑判据）

QEMU 冷启动 → `power-shell timeout -o 3600000` → 等 ~3.5 分钟 →
`snapshot_display` = 65,000+ 色（mean RGB≈[120,159,203]）。红线：勿删 guest
`/data/egl_no_init`（CPU 渲染开关）、勿 kill render_service、图形组件变更后必须冷启动。
