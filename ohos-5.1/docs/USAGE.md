# OpenHarmony 5.1.0 x86_general 显示修复镜像（20261020-displayfix）

> 本包为可直接部署的完整 OpenHarmony 镜像，包含「截图非全黑」任务的全部修复。
> 产出方式：从已验证可用的启动盘（QEMU 实测 65,000+ 色锁屏 UI 截图）逐分区提取，非源码全量重编。

## 1. 包内容与校验（见 md5.txt）

| 文件 | 说明 |
|------|------|
| system.img (2016MiB) | system 分区（ext4，**注意：guest 的 /vendor 实际也在本分区的 /vendor 目录**，sda3 分区未被挂载使用） |
| vendor.img (512MiB) | vendor 分区（运行时未使用，保留盘结构兼容） |
| userdata.img (1536MiB) | data 分区（**含必需 marker `/data/egl_no_init`**，勿删） |
| bzImage / ramdisk.img / grub.cfg | ESP 引导文件（内核 6.6.86） |
| disk_assemble.sh | 从本包重拼可启动 QEMU 磁盘的脚本 |
| final_qa.jpeg | 出厂验证截图（65,453 色） |

## 2. 镜像内嵌修复清单（相对官方 OpenHarmony-5.1.0-Release）

| # | 组件 | 修复 | 位置/md5 |
|---|------|------|----------|
| 1 | Mesa 图形栈 | 10 个库用 `-fno-emulated-tls` 重编（治 OHOS clang 下 glGetString 恒 null） | /system/lib64/{libEGL.so.1.0.0,libGLESv2.so.2.0.0,libGLESv1_CM.so.1.1.0,libglapi.so.0.0.0,libgbm.so.1.0.0}、/system/lib64/dri/libgallium_dri.so、/system/vendor/lib64/chipsetsdk/{libEGL_impl.so,libGLESv1/2/3_impl.so} |
| 2 | EGL wrapper | v4：HiLogPrint 本地 stub + BlobCache 四处短路（治 appspawn 沙箱懒加载 GOT 崩溃环）+ **marker 门控的 eglInitialize 强制失败**（`/data/egl_no_init` 存在即生效 → 全系统 CPU 渲染） | /system/lib64/platformsdk/libEGL.so（md5 70e0e669…） |
| 3 | 内核 virtio_gpu | 主平面格式表补 DRM_FORMAT_HOST_ARGB8888（治 composer AR24 atomic commit EINVAL） | /system/lib/modules/virtio-gpu.ko（md5 3d308b53…） |
| 4 | composer HAL | ApplyPlane 补 SRC_*/CRTC_* 几何属性（原缺失 → scanout 0x0）+ 可选 fb 镜像旁路（marker `/data/hdi_mirror`，**本包默认未启用**） | /system/vendor/lib64/libdisplay_composer_vendor.z.so（md5 50497250…） |
| 5 | vsync | drmWaitVBlank 失败软 60Hz 回退（virtio 无硬件 vblank） | 同上 composer HAL 内 |
| 6 | 诊断软链 | /system/lib/ 下 5 条软链（wrapper dlopen mesa 前提） | libc.so/libdrm.so/libglapi.so.0/libhilog.so/libsurface.z.so |

源码对应（全部在 /root/OpenHarmony/5.1-Release 源码树）：
- 内核：kernel/linux/linux-6.6 与 out/.../src_tmp/linux-6.6 的 drivers/gpu/drm/virtio/virtgpu_plane.c
- wrapper：foundation/graphic/graphic_2d/frameworks/opengl_wrapper/src/EGL/（egl_wrapper_loader/display/blob_cache）
- composer：device/soc/opc/x86_general/hardware/display/src/display_device/
- 增量编译：`cd out/x86_general && /root/oh_env/bin/ninja -w dupbuild=warn graphic/graphic_2d/libEGL.so` / `opc_soc_products/x86_general/libdisplay_composer_vendor.z.so`

## 3. 启动方法（QEMU）

```bash
qemu-system-x86_64 -machine pc -cpu max -m 8192 -smp 8 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -drive file=ohos_disk.img,format=raw,if=ide \
  -device virtio-gpu-pci \
  -netdev user,id=n1,hostfwd=tcp::5555-:55555 -device e1000,netdev=n1 \
  -display none -serial file:serial.log -monitor unix:qemu-mon.sock,server,nowait \
  -no-reboot
```
（无盘时先跑 `bash disk_assemble.sh ohos_disk.img` 拼盘；OVMF_VARS.fd 取 /usr/share/OVMF/OVMF_VARS_4M.fd 的一份拷贝）

## 4. 黄金启动公式（重要，已 3 次验证）

1. **QEMU 冷启动**（重启 QEMU 进程；不要在热环境里反复 guest reboot——appspawn 沙箱状态下 wrapper 会进崩溃环/挂起）
2. hdc 连接后执行 `power-shell timeout -o 3600000`（默认 30 秒灭屏，必须延长）
3. 等 ~3.5 分钟（SystemUI 起来）
4. 验证：`hdc shell snapshot_display -f /data/local/tmp/x.jpeg && hdc file recv ...` → 应为 65,000+ 色（mean RGB ≈ [120,159,203] 蓝色锁屏）

hdc：`/root/OpenHarmony/5.1-Release/prebuilts/ohos-sdk/linux/18/toolchains/hdc`，QEMU 重启后需 `hdc tconn 127.0.0.1:5555`。

## 5. 已知限制（交接必读）

1. **硬件屏（QEMU screendump）仍显示 console 残影**：用户态 dumb buffer 的 CPU 写入与 QEMU 资源 iov 指向内存不一致（页分裂），且 fb_write 无 deferred-io damage。snapshot（RS 内部渲染）不受影响。备选显示旁路（composer fb 镜像 + console poke，曾验证 91% 像素上屏）已在 HAL 内、touch `/data/hdi_mirror` 即启用
2. **绝不要 kill render_service**：SystemUI 渲染态不可恢复（vsync/会话链断裂），唯一恢复法 = QEMU 冷启动。杀 composer_host 也大概率触发同类停摆
3. **GL/GPU 渲染路径写入丢失**（本镜像全系统 CPU 渲染的原因）：任何走 EGL surface 输出的客户端像素会消失；glReadPixels/软渲计算正常。若未来修 iov 页分裂，可 rm /data/egl_no_init 恢复 GL 路径
4. 显示管线机制详情、排查工具与全部证据见交付目录 docs/ 与 tools/

## 6. 快速回归项

- 截图：上节第 4 步（≥60,000 色）
- 网络：guest 内 `ping 10.0.2.2`（e1000 + slirp）
- 模块：`lsmod | grep virtio_gpu`（virtio_gpu/drm_shmem_helper/virtio_dma_buf + e1000）
- 屏幕 dump：`screendump` 走 QEMU monitor（socket），当前已知为 2 色残影（见限制 1）
