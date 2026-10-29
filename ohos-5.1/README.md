# OpenHarmony 5.1 x86_general（显示修复版）构建指南

> 状态：已交付（2026-10-20 验证，2026-10-29 复核）。截图验证通过，锁屏界面
> 全像素统计 65,000+ 色；成品磁盘经过多次重启 QEMU 的端到端复验。本目录保存
> 全部补丁与文档，供复现使用。

## 快速开始

1. 获取源码（流程与 6.1 相同，换用 5.1 的 tag）：
   ```bash
   repo init -u https://gitee.com/openharmony/manifest.git \
       -b refs/tags/OpenHarmony-v5.1.0-Release --no-repo-verify && repo sync -c -j8
   ```
   opc 三个仓库的基线（OpenHarmony-5.1.0-Release 分支，gitee.com/diemit）：
   vendor_opc `23d66b68` / device_board_opc `fbf60291` / device_soc_opc `32fb2452`
2. Ubuntu 24.04 兼容：宿主机侧的 GN 修复见 docs/构建与部署手册.md 第 C 节。
3. 打补丁（按顺序应用）：
   ```bash
   bash <trio>/vendor/opc/x86_general/patches/patch.sh     # opc 仓库自带
   for p in patches/0{1,2,3,4,5,6}-*.patch; do git -C <对应仓库> apply $p; done
   cp -r patches/untracked/* <树>/device/{board,soc}/opc/...   # 见 patches/README.md
   ```
   mesa 的 `-fno-emulated-tls` 不在 git 补丁里（它注入在 meson 生成的
   build.ninja 编译规则中）：在 meson configure 之后、ninja 编译之前执行
   `bash patches/apply-emutls-flag.sh <树>/third_party/mesa3d`（脚本可重复执行）。
4. `./build.sh --product-name x86_general --ccache`（python3.10 虚拟环境）
5. 组装磁盘：`bash scripts/disk_assemble.sh ./disk.img`（分区镜像放在脚本同目录）
6. 启动与验证：`bash ../common/launch_qemu.sh ./disk.img`，启动后按下方
   步骤截图验证。

## 启动与截图验证

1. 重新启动 QEMU 进程（不要在系统已运行的状态下反复在虚拟机内 reboot，
   原因见 docs/USAGE.md 的已知限制）。
2. hdc 连接后执行 `power-shell timeout -o 3600000`，延长自动灭屏时间。
3. 等待约 3.5 分钟（SystemUI 启动完成）。
4. `bash ../common/verify_snapshot.sh out.jpeg`（默认用 SDK 18 的 hdc），
   全像素统计应为 65,000+ 色，mean RGB ≈ [120,159,203]。
   注意 `/data/egl_no_init` 是 CPU 渲染的开关，成品镜像依赖它，不要删除
   （机制说明见 docs/USAGE.md；6.1 不使用这个机制）。

## 文档索引

| 文档 | 内容 |
|------|------|
| docs/USAGE.md | 交付说明：镜像内嵌的修复清单、QEMU 命令、验证步骤、已知限制 |
| docs/根因分析_截图全黑.md | 截图全黑的根因分析（GL 写入丢失 / iov 内存不一致 / fbcon） |
| docs/构建与部署手册.md | 部署 / 增量编译 / 编译开关 |
| docs/HANDOFF_截图非全黑任务.md | 完整排查过程记录 |
| patches/README.md | 补丁的依赖顺序、应用命令、untracked 新文件的去处 |
