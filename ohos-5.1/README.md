# OpenHarmony 5.1 x86_general（显示修复版）构建指南

> 状态：已交付（2026-10-20 验证，2026-10-29 复核）。截图验证通过（全像素
> 65,000+ 色锁屏 UI），成品盘经多次冷启动端到端复验。本目录固化全部补丁
> 与文档供复现。

## 快速开始

1. 源码获取（与 6.1 流程相同，换 tag）：
   ```bash
   repo init -u https://gitee.com/openharmony/manifest.git \
       -b refs/tags/OpenHarmony-v5.1.0-Release --no-repo-verify && repo sync -c -j8
   ```
   opc 三仓基线（OpenHarmony-5.1.0-Release 分支，gitee.com/diemit）：
   vendor_opc `23d66b68` / device_board_opc `fbf60291` / device_soc_opc `32fb2452`
2. Ubuntu 24.04 兼容：host 侧 GN 修复见 docs/构建与部署手册.md §C。
3. 打补丁（顺序应用）：
   ```bash
   bash <trio>/vendor/opc/x86_general/patches/patch.sh     # opc 自带
   for p in patches/0{1,2,3,4,5,6}-*.patch; do git -C <对应仓库> apply $p; done
   cp -r patches/untracked/* <树>/device/{board,soc}/opc/...   # 见 patches/README.md
   ```
   mesa 的 `-fno-emulated-tls` 不在 git diff 内（注入在 meson 生成的
   build.ninja 编译规则上）：meson configure 之后、ninja 之前执行
   `bash patches/apply-emutls-flag.sh <树>/third_party/mesa3d`（幂等）。
4. `./build.sh --product-name x86_general --ccache`（python3.10 venv）
5. 拼盘：`bash scripts/disk_assemble.sh ./disk.img`（分区镜像放脚本同目录）
6. 启动与验证：`bash ../common/launch_qemu.sh ./disk.img`，启动后按下方
   步骤截图验证。

## 启动与截图验证

1. QEMU 冷启动（不要在热环境里反复 guest reboot，原因见 docs/USAGE.md 已知限制）。
2. hdc 连接后执行 `power-shell timeout -o 3600000` 延长灭屏时间。
3. 等约 3.5 分钟（SystemUI 就绪）。
4. `bash ../common/verify_snapshot.sh out.jpeg`（默认用 SDK 18 的 hdc），
   全像素统计应为 65,000+ 色，mean RGB ≈ [120,159,203]。
   注意 `/data/egl_no_init` 是 CPU 渲染开关，成品镜像依赖它，勿删（机制
   说明见 docs/USAGE.md；6.1 不适用此机制）。

## 文档索引

| 文档 | 内容 |
|------|------|
| docs/USAGE.md | 交付说明：镜像内嵌修复清单、QEMU 命令、验证步骤、已知限制 |
| docs/根因分析_截图全黑.md | 截图全黑的根因分析（GL 写入丢失 / iov 页分裂 / fbcon） |
| docs/构建与部署手册.md | 部署 / 增量编译 / 编译开关 |
| docs/HANDOFF_截图非全黑任务.md | 完整排查过程记录 |
| patches/README.md | 补丁依赖序、应用命令、untracked 新文件去处 |
