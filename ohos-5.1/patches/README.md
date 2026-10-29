# 5.1 源码补丁集

基于 OpenHarmony-5.1.0-Release + opc 三仓（OpenHarmony-5.1.0-Release 分支）
源码树的全部工作区改动。应用到干净的 5.1 多仓树后按
`../docs/构建与部署手册.md` 编译，即得到与交付镜像等价的产物。

## 补丁清单（按依赖序应用）

| # | 文件 | 目标仓库 | 内容 |
|---|------|----------|------|
| 01 | `01-kernel-6.6-virtio-gpu-AR24+damage-fallback.patch` | kernel/linux/linux-6.6 | virtio_gpu 主平面格式表加 ARGB8888（OHOS BGRA client buffer 被 EINVAL 拒绝的根修）+ 无 damage commit 全屏兜底 |
| 02 | `02-graphic_2d-opengl-wrapper+rs-fixes.patch` | foundation/graphic/graphic_2d | ① wrapper v4：HiLogPrint 本地 stub + BlobCache 四处短路（appspawn 沙箱懒加载 GOT 崩溃环根修）② marker 门控 eglInitialize 强制失败（`/data/egl_no_init` 存在时全系统 CPU 渲染，截图非黑的关键）③ render_context glGetString 空指针防护与 pbuffer 修复 ④ capture/main_thread 等修复 ⑤ graphic.cfg 增加全局 GL 环境变量 |
| 03 | `03-device-soc-opc-display-hal.patch` | device/soc/opc | composer HAL：ApplyPlane 补 SRC_*/CRTC_* 几何（原缺失导致 scanout 0x0）、drmWaitVBlank 失败软 60Hz 回退、可选 fb 镜像旁路（marker `/data/hdi_mirror`）、SetLayerBuffer 诊断钩子，及 gpu/BUILD.gn mesa 打包规则 |
| 04 | `04-device-board-opc-kernel-modules+init.patch` | device/board/opc | build_kernel.sh 启用 4 个 ko 单目标联编（dma_buf→shmem→gpu 依赖链）、init cfg 加载顺序、BUILD.gn 集成 |
| 05 | `05-mesa3d-fno-emulated-tls-build.patch` | third_party/mesa3d | x86 产品 swrast-only 配置与构建脚本修复（panfrost→swrast、无 LLVM） |
| 06 | `06-kernel-accesstokenid-doublefree.patch` | kernel/linux/linux-6.6 | accesstokenid 驱动删根节点悬垂指针根修（上游 bug：写回按值局部参数地址导致 g_token_perm_root 悬垂、双重释放）。5.1 用户态未触发；6.1 不打此补丁会在开机约 557s 出现 `BUG at mm/slub.c:448` |

## untracked/（新文件，直接拷入树）

| 路径 | 去处（相对源码树根） | 说明 |
|------|---------------------|------|
| `modules_gpu/` | device/board/opc/x86_general/ | 4 个 ko 的 GN 打包（BUILD.gn + ko 副本） |
| `modules_net/` | device/board/opc/x86_general/ | e1000.ko 打包 |
| `zz_net_setup.cfg` | device/board/opc/x86_general/cfg/ | 网络固化配置 |
| `gpu-chipsetsdk-libs/` | device/soc/opc/x86_general/hardware/gpu/lib64/chipsetsdk/ | 重编后的 mesa impl 库（二进制，也可从 ../../tools/mesa-libs/ 取） |

## 应用方法

mesa 的 `-fno-emulated-tls` 不在 git diff 内（注入在 meson 生成的编译规则上），
meson configure 之后、ninja 编译之前执行：

```bash
bash <本目录>/apply-emutls-flag.sh <源码树>/third_party/mesa3d   # 幂等
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
本补丁集是其工作区快照，用于向其他环境分发。
