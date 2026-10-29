# 6.1 补丁集索引

共三组补丁，由 `../scripts/apply-x86-port-patches.sh` 按顺序应用，无需手工
执行。

## opc-trio/（opc 三个仓库的增量，基于上级 README 第 1.1 节的基线 commit）

| # | 文件 | 目标仓库 | 内容 |
|---|------|----------|------|
| 01 | vendor-config-x86port | vendor/opc | 产品配置：hdf 子系统补 drivers_adapter（hdf_devhost 的来源）、deps_guard 白名单等 |
| 02 | board-kernel-modules-cfg | device/board/opc | 内核模块配置与 4 个内核模块的联合编译方案 |
| 03 | soc-display-hal-elfutils-lzma | device/soc/opc | elfutils/lzma 构建修复 |
| 04 | soc-display-hal-port-from-51 | device/soc/opc | 5.1 显示 HAL 的移植：ApplyPlane 几何、软件 vsync（含 worker 生命周期修复）、framebuffer 镜像旁路 |

## ohos-repos/（OpenHarmony 主干各仓，编号-<仓库路径>，路径中的 / 写作 -、_ 保留）

11-build、12-productdefine-common、13-third_party-libvpx、14-third_party-lzma、
15-third_party-toybox、16-third_party-skia、17-base-startup-appspawn、
18-base-useriam-user_auth_framework、19-base-usb-usb_manager、
20-foundation-arkui-ace_engine、21-foundation-graphic-graphic_2d（wrapper v4
的移植 + kmsg 诊断、RenderContextGL 修复、EGL surface 重试、vsync 探针）、
22-foundation-communication-netstack、23-developtools-integration_verification、
24-developtools-profiler、25-base-security-selinux_adapter（selabel_open
在无 file_contexts 时的崩溃修复）。

## untracked/（新增文件，直接拷入源码树）

board/ 目录（modules_gpu、modules_net 等）拷到 device/board/opc/，
soc/ 目录拷到 device/soc/opc/。
