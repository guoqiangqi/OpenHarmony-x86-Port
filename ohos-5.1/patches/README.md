# 源码补丁集（20261020-displayfix）

> 基于 OpenHarmony-5.1.0-Release + vendor_opc（OpenHarmony-5.1.0-Release 分支）源码树的全部工作区改动。
> 应用对象：干净的 5.1-Release 多仓树（repo init -b refs/tags/OpenHarmony-v5.1.0-Release + vendor_opc/device_board_opc/device_soc_opc 三仓）。
> 应用后按 `../docs/构建与部署手册.md` §B/§C 编译；编译产物等价于 `../images/` 中的成品。

## 补丁清单（按依赖序应用）

| # | 文件 | 仓库 | 内容 |
|---|------|------|------|
| 01 | `01-kernel-6.6-virtio-gpu-AR24+damage-fallback.patch` | kernel/linux/linux-6.6 | virtio_gpu 主平面格式表加 ARGB8888（OHOS BGRA client buffer 被 EINVAL 拒绝的根修）+ 无 damage commit 全屏兜底 |
| 02 | `02-graphic_2d-opengl-wrapper+rs-fixes.patch` | foundation/graphic/graphic_2d | ① wrapper v4：HiLogPrint 本地 stub + BlobCache 四处短路（appspawn 沙箱懒加载 GOT 崩溃环根修）② **marker 门控 eglInitialize 强制失败**（`/data/egl_no_init`→全系统 CPU 渲染=截图非黑的关键）③ render_context glGetString 空指针防护 ④ capture/main_thread 等前序会话修复 |
| 03 | `03-device-soc-opc-display-hal.patch` | device/soc/opc | composer HAL：ApplyPlane 补 SRC_*/CRTC_* 几何（原缺失→scanout 0x0）、drmWaitVBlank 失败软 60Hz 回退、可选 fb 镜像旁路（marker `/data/hdi_mirror`）、SetLayerBuffer 诊断钩子 + gpu/BUILD.gn（mesa 新库打包规则） |
| 04 | `04-device-board-opc-kernel-modules+init.patch` | device/board/opc | build_kernel.sh 启用 4 个 ko 单目标联编（dma_buf→shmem→gpu 依赖链）+ init cfg syncexec 加载顺序 + BUILD.gn 集成 |
| 05 | `05-mesa3d-fno-emulated-tls-build.patch` | third_party/mesa3d | x86 产品 swrast-only 配置 + 构建脚本修复（panfrost→swrast、无 LLVM） |
| 06 | `06-kernel-accesstokenid-doublefree.patch` | kernel/linux/linux-6.6 | accesstokenid 驱动删根节点悬垂指针根修（上游 bug，5.1 未触发、6.1 开机 ~557s `BUG at mm/slub.c:448` panic；`remove_node_by_token` 根节点情形写回局部参数地址 → `g_token_perm_root` 悬垂 → 双重释放）。含 slub_debug=UFZ 证据链 |

## untracked/（新文件，直接拷入树）

| 路径 | 去处（相对源码树根） | 说明 |
|------|---------------------|------|
| `modules_gpu/` | device/board/opc/x86_general/ | 4 个 ko 的 GN 打包（BUILD.gn + ko 副本） |
| `modules_net/` | device/board/opc/x86_general/ | e1000.ko 打包 |
| `zz_net_setup.cfg` | device/board/opc/x86_general/cfg/ | 网络固化配置 |
| `gpu-chipsetsdk-libs/` | device/soc/opc/x86_general/hardware/gpu/lib64/chipsetsdk/ | 重编后的 mesa impl 库（二进制，也可从 `../tools/mesa/new/` 取） |

## 应用方法

**mesa 的 `-fno-emulated-tls` 不在 git diff 里**（它注入在 meson 生成的 build.ninja 编译 rule 上）。meson configure 之后、ninja 编译之前执行：

```bash
bash <本目录>/apply-emutls-flag.sh <源码树>/third_party/mesa3d   # 幂等，已在镜像产物中生效
```

```bash
cd <源码树根>
git -C kernel/linux/linux-6.6        apply <本目录>/01-*.patch
git -C foundation/graphic/graphic_2d apply <本目录>/02-*.patch
git -C device/soc/opc                apply <本目录>/03-*.patch
git -C device/board/opc              apply <本目录>/04-*.patch
git -C third_party/mesa3d            apply <本目录>/05-*.patch
git -C kernel/linux/linux-6.6        apply <本目录>/06-*.patch
cp -r <本目录>/untracked/modules_gpu <本目录>/untracked/modules_net device/board/opc/x86_general/
cp <本目录>/untracked/zz_net_setup.cfg device/board/opc/x86_general/cfg/
cp -r <本目录>/untracked/gpu-chipsetsdk-libs device/soc/opc/x86_general/hardware/gpu/lib64/chipsetsdk
```

注意：本机源码树 `/root/OpenHarmony/5.1-Release` 已包含全部改动（未 commit），本补丁集是其工作区快照，用于向其他项目树分发。
