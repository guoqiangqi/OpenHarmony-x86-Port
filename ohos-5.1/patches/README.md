# 5.1 源码补丁集

内容是基于 OpenHarmony-5.1.0-Release + opc 三个仓库
（OpenHarmony-5.1.0-Release 分支）源码树的全部工作区改动。应用到干净的
5.1 多仓源码树后，按 `../docs/构建与部署手册.md` 编译，即可得到与交付镜像
等价的产物。

## 补丁清单（按依赖顺序应用）

| # | 文件 | 目标仓库 | 内容 |
|---|------|----------|------|
| 01 | `01-kernel-6.6-virtio-gpu-AR24+damage-fallback.patch` | kernel/linux/linux-6.6 | virtio_gpu 主平面格式表加 ARGB8888（OHOS 的 BGRA client buffer 被 EINVAL 拒绝的根本原因），另加无 damage commit 的全屏刷新兜底 |
| 02 | `02-graphic_2d-opengl-wrapper+rs-fixes.patch` | foundation/graphic/graphic_2d | ① wrapper v4：HiLogPrint 本地 stub + BlobCache 四处短路（解决 appspawn 沙箱内懒加载 GOT 引发的崩溃循环）② 由标记文件控制的 eglInitialize 强制失败（`/data/egl_no_init` 存在时全系统走 CPU 渲染，截图不黑的关键）③ render_context 的 glGetString 空指针防护与 pbuffer 修复 ④ capture/main_thread 等修复 ⑤ graphic.cfg 增加全局 GL 环境变量 |
| 03 | `03-device-soc-opc-display-hal.patch` | device/soc/opc | composer HAL：ApplyPlane 补齐 SRC_*/CRTC_* 几何属性（原来缺失，导致 scanout 尺寸为 0）、drmWaitVBlank 失败时回退软件 60Hz、可选的 framebuffer 镜像旁路（标记文件 `/data/hdi_mirror`）、SetLayerBuffer 诊断钩子，以及 gpu/BUILD.gn 的 mesa 打包规则 |
| 04 | `04-device-board-opc-kernel-modules+init.patch` | device/board/opc | build_kernel.sh 启用 4 个内核模块的单目标联合编译（dma_buf、shmem、gpu 依次依赖）、init cfg 的加载顺序、BUILD.gn 集成 |
| 05 | `05-mesa3d-fno-emulated-tls-build.patch` | third_party/mesa3d | x86 产品的 swrast-only 配置与构建脚本修复（panfrost 改为 swrast、去掉 LLVM） |
| 06 | `06-kernel-accesstokenid-doublefree.patch` | kernel/linux/linux-6.6 | accesstokenid 驱动删除根节点时的悬垂指针修复（上游缺陷：把指针写回按值传入的局部参数地址，导致 g_token_perm_root 悬垂和双重释放）。5.1 用户态没有触发；6.1 不打此补丁会在开机约 557 秒出现 `BUG at mm/slub.c:448` |
| 07 | `07-kernel-fs-verity-enable.patch` | kernel/linux/linux-6.6 | fs-verity 支持：从 6.15 回移的最小 /sys/fs/verity 接口（sysfs.c 新文件 + init 挂接）。配合补丁 04 里 oh_defconfig 的 `CONFIG_FS_VERITY=y`（内核配置开关）一起生效。官方应用签名链 HAP 安装（错误码 9568393）的内核侧依赖 |
| 08 | `08-code-sign-x86-statx-nr.patch` | base/security/code_signature | `IsSupportFsVerity` 使用架构正确的 SYS_statx（x86_64=332）。原先经 `<asm/unistd.h>` 误用 asm-generic/aarch64 的 291，在 x86_64 上变成 `epoll_create1` → errno=22 → 安装 9568393。ioctl/FEATURE_R15 通过不能单独当作签名安装可用 |
| 09 | `09-graphic_2d-flutter-surface-forcecpu.patch` | foundation/graphic/graphic_2d | Flutter/XComponent SURFACE 在 `/data/egl_no_init` CPU 合成下强制 `CreateBufferDrawParam(..., forceCPU=true)`，走 `DrawBuffer(CPU)` 把 producer buffer 画进 uni-render（否则 EGLImage/OES 采样为空 → 白屏）。需配合 `rosen.client_composition.enabled=1` 与 HAP `--enable-software-rendering`；说明见 `../docs/FLUTTER-GRAPHICS.md` |

## untracked/（新增文件，直接拷入源码树）

| 路径 | 去处（相对源码树根） | 说明 |
|------|---------------------|------|
| `modules_gpu/` | device/board/opc/x86_general/ | 4 个内核模块的 GN 打包（BUILD.gn + 模块副本） |
| `modules_net/` | device/board/opc/x86_general/ | e1000.ko 打包 |
| `zz_net_setup.cfg` | device/board/opc/x86_general/cfg/ | 网络固化配置 |
| `gpu-chipsetsdk-libs/` | device/soc/opc/x86_general/hardware/gpu/lib64/chipsetsdk/ | 重编后的 mesa impl 库（二进制，也可从 ../../tools/mesa-libs/ 获取） |

## 应用方法

mesa 的 `-fno-emulated-tls` 不在 git 补丁里（它注入在 meson 生成的编译规则
上），需要在 meson configure 之后、ninja 编译之前执行：

```bash
bash <本目录>/apply-emutls-flag.sh <源码树>/third_party/mesa3d   # 可重复执行
```

```bash
cd <源码树根>
git -C kernel/linux/linux-6.6        apply <本目录>/01-*.patch
git -C kernel/linux/linux-6.6        apply <本目录>/06-*.patch
git -C kernel/linux/linux-6.6        apply <本目录>/07-*.patch
git -C foundation/graphic/graphic_2d apply <本目录>/02-*.patch
git -C foundation/graphic/graphic_2d apply <本目录>/09-*.patch
git -C device/soc/opc                apply <本目录>/03-*.patch
git -C device/board/opc              apply <本目录>/04-*.patch
git -C third_party/mesa3d            apply <本目录>/05-*.patch
git -C base/security/code_signature  apply <本目录>/08-*.patch
cp -r <本目录>/untracked/modules_gpu <本目录>/untracked/modules_net device/board/opc/x86_general/
cp <本目录>/untracked/zz_net_setup.cfg device/board/opc/x86_general/cfg/
cp -r <本目录>/untracked/gpu-chipsetsdk-libs device/soc/opc/x86_general/hardware/gpu/lib64/chipsetsdk
```

启用 fs-verity 后注意两件事：内核配置变更改变了模块 ABI，4 个内核模块
（virtio-gpu、virtio_dma_buf、drm_shmem_helper、e1000）必须与新 bzImage 一起
重新编译；用旧分区镜像拼盘时还需给 data 分区补 ext4 verity 特性位并把
key_enable.cfg 的 require_signatures 置 0，`../scripts/enable-fsverity-disk.sh`
把这三步打包成了一条命令。

备注：本机源码树 `/root/OpenHarmony/5.1-Release` 已包含全部改动（未 commit），
本补丁集是它的工作区快照，用于向其他环境分发。
