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
git -C foundation/graphic/graphic_2d apply <本目录>/02-*.patch
git -C device/soc/opc                apply <本目录>/03-*.patch
git -C device/board/opc              apply <本目录>/04-*.patch
git -C third_party/mesa3d            apply <本目录>/05-*.patch
cp -r <本目录>/untracked/modules_gpu <本目录>/untracked/modules_net device/board/opc/x86_general/
cp <本目录>/untracked/zz_net_setup.cfg device/board/opc/x86_general/cfg/
cp -r <本目录>/untracked/gpu-chipsetsdk-libs device/soc/opc/x86_general/hardware/gpu/lib64/chipsetsdk
```

备注：本机源码树 `/root/OpenHarmony/5.1-Release` 已包含全部改动（未 commit），
本补丁集是它的工作区快照，用于向其他环境分发。
