# OpenHarmony 5.1.0 x86_general 显示修复镜像（20261020-displayfix）

> 本包是可直接部署的完整 OpenHarmony 镜像，包含截图全黑问题的全部修复。
> 镜像从已验证可用的启动盘中逐分区提取（QEMU 实测截图 65,000+ 色的锁屏界面），
> 不是源码全量重编的产物。

## 1. 包内容与校验（见 md5.txt）

| 文件 | 说明 |
|------|------|
| system.img (2016MiB) | system 分区（ext4）。注意 guest 的 /vendor 实际也在本分区的 /vendor 目录，sda3 分区未被挂载使用 |
| vendor.img (512MiB) | vendor 分区（运行时未使用，为保持盘结构兼容而保留） |
| userdata.img (1536MiB) | data 分区，包含必需的标记文件 /data/egl_no_init，不要删除 |
| bzImage / ramdisk.img / grub.cfg | ESP 引导文件（内核 6.6.86） |
| disk_assemble.sh | 从本包重新组装可启动 QEMU 磁盘的脚本 |
| final_qa.jpeg | 出厂验证截图（65,453 色） |

## 2. 镜像内嵌的修复清单（相对官方 OpenHarmony-5.1.0-Release）

| # | 组件 | 修复 | 位置/md5 |
|---|------|------|----------|
| 1 | Mesa 图形栈 | 10 个库用 `-fno-emulated-tls` 重编（解决 OHOS clang 下 glGetString 始终返回 null） | /system/lib64/{libEGL.so.1.0.0,libGLESv2.so.2.0.0,libGLESv1_CM.so.1.1.0,libglapi.so.0.0.0,libgbm.so.1.0.0}、/system/lib64/dri/libgallium_dri.so、/system/vendor/lib64/chipsetsdk/{libEGL_impl.so,libGLESv1/2/3_impl.so} |
| 2 | EGL wrapper | v4：HiLogPrint 本地 stub + BlobCache 四处短路（解决 appspawn 沙箱内懒加载 GOT 引发的崩溃循环）；另有由标记文件控制的 eglInitialize 强制失败（/data/egl_no_init 存在时生效，全系统回退 CPU 渲染） | /system/lib64/platformsdk/libEGL.so（md5 70e0e669…） |
| 3 | 内核 virtio_gpu | 主平面格式表补上 DRM_FORMAT_HOST_ARGB8888（解决 composer 的 AR24 atomic commit 被拒返回 EINVAL） | /system/lib/modules/virtio-gpu.ko（md5 3d308b53…） |
| 4 | composer HAL | ApplyPlane 补齐 SRC_*/CRTC_* 几何属性（原来缺失，导致 scanout 尺寸为 0）；可选的 framebuffer 镜像旁路（标记文件 /data/hdi_mirror，本包默认未启用） | /system/vendor/lib64/libdisplay_composer_vendor.z.so（md5 50497250…） |
| 5 | vsync | drmWaitVBlank 失败时回退到软件 60Hz（virtio 没有硬件 vblank） | 同上 composer HAL 内 |
| 6 | 诊断软链 | /system/lib/ 下 5 条软链（wrapper dlopen mesa 的前提） | libc.so/libdrm.so/libglapi.so.0/libhilog.so/libsurface.z.so |

源码对应关系（全部在 /root/OpenHarmony/5.1-Release 源码树）：
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
（还没有磁盘时，先执行 `bash disk_assemble.sh ohos_disk.img` 组装；OVMF_VARS.fd
从 /usr/share/OVMF/OVMF_VARS_4M.fd 复制一份）

## 4. 标准启动流程（重要，已验证 3 次）

1. 重新启动 QEMU 进程。不要在系统已运行的状态下反复在虚拟机内 reboot——
   appspawn 沙箱状态下 wrapper 会进入崩溃循环或挂起。
2. hdc 连接后执行 `power-shell timeout -o 3600000`（默认 30 秒灭屏，必须延长）。
3. 等待约 3.5 分钟（SystemUI 启动完成）。
4. 验证：`hdc shell snapshot_display -f /data/local/tmp/x.jpeg && hdc file recv ...`，
   全像素统计应为 65,000+ 色（mean RGB ≈ [120,159,203]，蓝色锁屏界面）。

hdc 路径：`/root/OpenHarmony/5.1-Release/prebuilts/ohos-sdk/linux/18/toolchains/hdc`。
QEMU 重启后需要重新执行 `hdc tconn 127.0.0.1:5555`。

## 5. 已知限制（交接必读）

1. 硬件屏（QEMU screendump）仍显示 console 残影：用户态对 dumb buffer 的 CPU
   写入与 QEMU 资源 iov 指向的内存不一致，且 fb_write 不标记 deferred-io
   damage。snapshot 走 RenderService 内部渲染，不受影响。备选的显示旁路
   （composer 把内容镜像到 framebuffer，再借 console 输出触发推送，曾验证
   91% 像素能显示到屏幕）已实现在 HAL 内，创建 /data/hdi_mirror 即启用。
2. 不要结束 render_service 进程：SystemUI 的渲染状态会因此不可恢复（vsync 与
   会话链断裂），唯一的恢复办法是重新启动 QEMU。结束 composer_host 大概率
   也会引发同样的停摆。
3. GL/GPU 渲染路径存在写入丢失（这是本镜像让全系统走 CPU 渲染的原因）：任何
   通过 EGL surface 输出的客户端像素都会消失；glReadPixels 和软件渲染的计算
   本身正常。将来若解决了 iov 内存不一致问题，可删除 /data/egl_no_init 恢复
   GL 路径。
4. 显示管线的机制细节、排查工具与全部证据见交付目录的 docs/ 与 tools/。

## 6. 快速回归检查

- 截图：上一节第 4 步（不少于 60,000 色）
- 网络：guest 内 `ping 10.0.2.2`（e1000 + slirp）
- 内核模块：`lsmod | grep virtio_gpu`（应看到 virtio_gpu/drm_shmem_helper/virtio_dma_buf 和 e1000）
- 屏幕 dump：`screendump` 走 QEMU monitor（socket），目前已知是 2 色残影（见限制 1）
