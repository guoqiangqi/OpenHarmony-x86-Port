# 交接文档：OpenHarmony x86_general 镜像「截图非全黑」任务

> 最后更新：2026-10-20 14:15（第十五轮排查结束，主线目标已达成）。
> 本文档用于交接，后续接手的人可从「当前状态」一节直接续上。
>
> 文中「基准状态」指 10-19 系统能正常显示彩色锁屏时的组件状态，用来和
> 当天的异常表现对照。

---

## 核心成果（2026-10-20 14:00 达成）

`snapshot_display` 截图得到 65,000+ 色的 OpenHarmony 蓝色锁屏界面
（mean RGB [120,159,203]），结果稳定、可复现。
证据：/root/SUCCESS_SNAPSHOT.jpeg、/root/f1.jpeg、/root/f3.jpeg
（三次独立冷启动，分别为 65889/65668/65247 色）。

### 可靠的启动流程（已验证 3 次，结果完全一致）

1. 磁盘上已部署：wrapper libEGL.so（md5 前缀 70e0e669，v4 基础上加了由
   标记文件控制的 eglInitialize 强制失败）、标记文件 `/data/egl_no_init`、
   composer vendor 库（md5 前缀 50497250，镜像旁路版，标记文件
   `/data/hdi_mirror`）、内核模块 virtio-gpu.ko（md5 前缀 3d308b53，AR24 版）
2. 重新启动 QEMU 进程（在虚拟机内 reboot 有时也有效，但 appspawn 的运行
   环境没有被重置时，可能出现崩溃循环）
3. hdc 连接后执行 `chmod 666 /dev/fb1`（镜像旁路需要）和
   `power-shell timeout -o 3600000`（防止 30 秒后自动灭屏）
4. 等待约 3.5 分钟（SystemUI 启动完成），执行
   `snapshot_display -f /data/local/tmp/x.jpeg`，结果应为 6.5 万色左右

### 根因分析（2026-10-20 完全确认，推翻了此前的多个假设）

- 根本原因是 GL/软件渲染的写入丢失。SystemUI 走 EGL/GL 路径时表面上看渲染
  正常（有帧率、有 surface，eglQuerySurface 报错但流程继续），但写入
  dumb buffer 支撑的 surface 的像素全部丢失，RenderService 合成时拿到的
  是空 buffer，最终全黑。CPU 路径（mmap 直接写）的像素完好，因此能显示
  彩色。证据：SystemUI 稳定走 GL 时画面为黑；eglInitialize 失败（自动回退
  CPU）时立即出现彩色。
- 前一天（10-19）出现彩色，是因为 SystemUI 的 EGL 初始化恰好失败（appspawn
  沙箱里 HiLogPrint GOT 懒加载缺陷的温和表现），系统自动回退到 CPU 渲染。
  10-20 反复还原文件却始终黑屏，是因为同一个缺陷在不同环境下表现为不同
  形态——温和失败、崩溃循环或挂起。形态取决于环境，但只要 EGL 初始化成功
  画面就是黑的，这一点始终不变。
- 修复方法：wrapper 中在标记文件存在时让 `EglWrapperDisplay::Init` 直接
  返回 EGL_FALSE（源码 egl_wrapper_display.cpp，可搜索 FORCE-INIT-FAIL），
  使所有客户端确定性地走 CPU 回退；同时需要 v4 的 HiLogPrint stub 和
  BlobCache 短路，否则失败路径自身会崩溃。
- 特别注意：不要重启 render_service。这会破坏 SystemUI 的渲染状态且无法
  恢复，唯一的恢复办法是再次冷启动。停止 composer_host 大概率也会引发
  同样的停摆。


---

## 0. 当前状态（2026-10-20 结束时）

- 已修复并确认：原来的卡点（AR24 EINVAL）。内核 virtio_gpu 主平面格式表
  补了 ARGB8888，composer 的 drmModeAtomicCommit 全部被接受（dmesg 无
  EINVAL，`handle 0xN, crtc 1280x800+0+0` 正常轮转）。
- 已确认：RenderService 到 composer 的内容链路打通。到达 composer
  `SetLayerBuffer` 的 client buffer 内是 98814 色的 OpenHarmony 蓝色锁屏
  界面（多次取证，dump 文件在 /root/layer_dump_view.png 等）。
- 已查明：显示最后一段链路的全部机制（见第 2 节 fbcon 的行为规则）。
  镜像写入 fb1、借 console 输出触发推送的显示通路，做过像素吻合度 91% 的
  闭环验证。
- 未解决：当天新出现的回归——SystemUI 全天 0 帧渲染
  （`fps ScreenLockWindow` = 0 0 0 0，snapshot 全黑，
  `hardwareVSyncStatus:0`）。把组件逐个还原到基准状态也无法恢复渲染
  （还原情况见第 5 节），判断是运行时或环境层的问题。
- 另外，前一轮的成果仍然有效：`/root/snap1.jpeg`（65303 色，
  mean [120,159,203]）证明 RenderService 层的截图可以非黑。

接手建议：直接从第 4 节的当日新回归入手，这是目前唯一挡住主线的问题。

---

## 1. 已修复并验证的问题（不必重复排查）

### 1.1 内核 AR24、几何属性与 damage（均已验证有效）

| # | 问题 | 修复 | 验证 |
|---|------|------|------|
| 1 | virtio_gpu 主平面格式表只有 XRGB8888（XR24），OHOS client buffer 是 BGRA（对应 DRM 的 AR24），atomic commit 报 EINVAL | `virtgpu_plane.c` 的 `virtio_gpu_formats[]` 加 `DRM_FORMAT_HOST_ARGB8888`（上游 6.6 本来就只有 XR24，属于扩展而非恢复） | commit 全部接受；plane state 显示 `fb=N format=AR24 allocated by OS_IPC_1_942` |
| 2 | composer 的 atomic commit 从不带 SRC_*/CRTC_* 几何属性，kernel 里 src/crtc 全为 0x0，scanout 尺寸为零 | HAL 的 `ApplyPlane` 补 8 个几何属性（SRC_W/H 为 16.16 定点） | dmesg 出现 `handle 0xN, crtc 1280x800+0+0, src 1280x800+0+0` |
| 3 | 同一 FB 无 damage 的 commit 被 virtio 驱动静默跳过（符合 DRM 语义） | `virtio_gpu_primary_plane_update` 的 damage_merged==false 分支改为全屏刷新兜底 | QEMU trace 可见 transfer/flush 每帧到达 |

- 内核补丁打在两棵树上：`kernel/linux/linux-6.6/`（干净树）与
  `out/x86_general/kernel/src_tmp/linux-6.6/`（实际编译树）。单目标补编命令
  （三个模块必须在同一次 make 调用里）：
  ```
  cd out/x86_general/kernel/src_tmp/linux-6.6
  export PATH=…/prebuilts/clang/ohos/linux-x86_64/llvm/bin:…/pahole/bin:$PATH
  export DEVICE_NAME=x86_general PRODUCT_COMPANY=opc PRODUCT_PATH=vendor/opc/x86_general
  make LLVM=1 LLVM_IAS=1 KBUILD_OUTPUT=out/x86_general/kernel/OBJ/linux-6.6 \
    drivers/virtio/virtio_dma_buf.ko drivers/gpu/drm/drm_shmem_helper.ko drivers/gpu/drm/virtio/virtio-gpu.ko -j$(nproc)
  ```
- 内核模块的版本现状（重要）：guest/镜像里的 `/system/lib/modules/virtio-gpu.ko`
  目前是 3d308b53，即仅含 AR24 修复的版本（当天回滚到基准状态时换回的）。
  含 damage 兜底的完整版是 058f2e3b（源码在树里，重新编译即可得到）。
  AR24 是主线必需；damage 兜底是可选增强。
- damage 兜底版曾长期部署，未发现问题；回滚只是还原变量实验的一部分，
  并不表示它有问题。

### 1.2 显示链路的关键事实（均已验证）

1. QEMU 的 screendump 反映的就是 virtio-gpu 的 scanout 内容（向 fb1 写 0
   后屏幕对应区域变黑，已验证）。
2. 用户态对 dumb buffer 的 CPU 写入（mmap dma-buf 与 MAP_DUMB 结果相同）
   与 QEMU 资源 iov 指向的内存不一致。用 drmprobe（源码 /root/drmprobe.c，
   guest 内 /data/local/tmp/drmprobe）写入梯度后调用 SetCrtc，QEMU 收到
   完整的 transfer/scanout/flush 命令流（trace 记录到 83/80/84 条），但
   画面不动；而内核 fbcon 自己绘制的内容显示正常。这个内存不一致的根因
   尚未定位（按源码核对，attach backing 的 sgt 与 mmap 应当同页，找不到
   分裂点），是遗留的深层问题。
3. `/dev/fb1` 的 write() 不会标记 deferred-io damage（fb_io_write 是纯
   memcpy，见内核源码），所以用户态写 fb1 永远不会主动显示到屏幕上。
4. fbcon 自身的重绘是把 fb1 内容（包括用户态写入的部分）送上屏幕的唯一
   机制：console 打印引起滚动，触发全屏 damage 并推送（22:18 的实验里，
   dd 写入导致全屏变黑，就是借助了 console 输出引发的重绘）。
5. composer 的 plane commit 与 fbcon 重绘互相争抢 plane：composer 每帧
   commit（尤其是带 damage 兜底时）会把 fbcon 刚推送的内容立即覆盖成
   iov 里的无效数据。

### 1.3 镜像旁路方案（已验证可行，被第 4 节的回归挡住）

- 改动位置：`device/soc/opc/x86_general/hardware/display/.../core/hdi_layer.cpp`
  的 `SetLayerBuffer` 内 `MirrorToFramebuffer()`（写 /dev/fb1），加上每帧向
  /dev/console 写 "\n" 触发 fbcon 全屏推送；`hdi_drm_composition.cpp` 的
  `Apply` 在 `/data/hdi_mirror` 存在时跳过 plane 循环（保留
  modeset/commit/outfence）。
- 注意：这些改动目前存放在 git stash 里（`git stash list` 可以看到
  `my-composer-changes`），工作树中是原始 HAL 加上此前的 vsync 补丁。
  需要时用 `git stash pop` 恢复。
- 曾达到的效果：镜像帧（SetLayerBuffer 收到的 UI buffer）写入 fb1，经
  console 输出触发推送后，screendump 与 fb1 内容的像素吻合度为 91%。
- 使用条件：/dev/fb1 需要 666 权限供 composer 访问（尝试过在 ueventd 里
  加规则，失败；手动 chmod 可行）；console 必须有输出才会推送
  （`dmesg -n 1` 静默之后需要主动写入触发）。

---

## 2. fbcon 的行为规则（避免重复踩坑）

- fbcon 处于绑定状态时，console 打印（包括开机阶段 init 的大量输出）会
  持续争抢 plane；解绑后 console 输出仍走串口，fb1 内容不再变化。
- 解绑后对 /dev/fb1 的写入同样不会显示到屏幕（当天多次启动验证）；只有 fbcon
  处于绑定且活跃状态时，写 fb1 的内容才会随重绘一起推送。
- 周期性输出的来源是 init 的 LoopEvent timeout 警告：多个服务的 epoll
  倒计时每秒打印一次，走 printk。执行 `dmesg -n 1` 可以一并静默串口
  和 fbcon。

---

## 3. 环境速查（沿用前版，均有效）

| 项 | 值 |
|---|------|
| QEMU 磁盘 | /root/ohos_disk_new.img（AR24 内核模块 + 与基准状态一致的库 + 原始 userdata）；/root/ohos_disk_fresh.img 是备用盘，用 10-18 的分区镜像组装并注入了其余增量（内核模块还是 10-18 原版 ed110b7d） |
| QEMU 启动 | 命令见 /root/.bash_history 或前版交接文档；`-s` 可留可去（实测无影响）；带了 `-no-reboot`，虚拟机内 reboot 后必须手动重启 QEMU 进程 |
| ninja | `cd out/x86_general && /root/oh_env/bin/ninja -w dupbuild=warn <target>`；composer HAL 目标：`opc_soc_products/x86_general/libdisplay_composer_vendor.z.so` |
| drm debug | guest 内执行 `echo 31 > /sys/module/drm/parameters/debug`；-95 的周期输出说明软件 vsync worker 在运行（注意每次启动都要重新打开 debug，且 dmesg 环形缓冲会被刷掉） |
| QEMU trace | 启动加 `-trace enable='virtio_gpu*'`，日志在 qemu_stdout.log（事件名：res_xfer_toh_2d / set_scanout / res_flush / res_create_2d / res_back_attach） |
| 探针 | /root/drmprobe.c（直接 ioctl 的 DRM 探针；编译：clang --target=x86_64-linux-ohos --sysroot=/root/drmprobe_sysroot -I/root/drmprobe_inc）；/root/wrapprobe（EGL/GL 探针，当天全部通过，说明 GL 栈本身健康） |

## 4. 当日新回归：SystemUI 0 帧（当前唯一未解决的问题）

症状（每次启动稳定复现）：
- `snapshot_display` = 纯黑（1 色）；`hidumper -s RenderService -a 'fps ScreenLockWindow'` = 0 0 0 0
- `hidumper -s RenderService -a allInfo` 显示 `hardwareVSyncStatus:0`（基准状态为 1）
- SystemUI 进程活着（TIME_TICK/控制中心日志正常），窗口树里有
  ScreenLockWindow（1280x800, z=4），但窗口 surface 从未提交过一帧
- composer 的 vsync worker（-95 周期输出）在启动后不运行；结束后重启
  composer，worker 恢复运行，但 RenderService 的采样仍是 0，回调链没有接上
- RenderService init 日志正常（RegRefreshCallback/-2 等错误与基准状态相同）；
  `power-shell dump -s` = AWAKE；DisplayPowerManager State=2（DIM）

已排除的方向（都试过，无效）：
- 回滚内核模块到 AR24-only（3d308b53，与基准状态同版本）
- 回滚 composer HAL 到原始（git stash，db200150，与基准状态 md5 相同）
- 恢复 10-18 原始 userdata（dd 分区镜像）
- 删除 ueventd 注入行、复位 OVMF_VARS、QEMU 加回 -s、逐字节复刻启动参数
- 全新组装磁盘（mkdisk2.sh + 10-18 镜像 + 注入全部基准状态增量：mesa 10 库/
  软链 5 条/v0 wrapper/vendor impl4）
- 各种结束服务的顺序（composer→RS、RS→composer、同时结束、单结束 SystemUI）
- GL 栈健康（wrapprobe 全部通过），排除 mesa 部署问题

尚未排除的方向（接手建议）：
1. RenderService 的 vsync 使能链：RenderService 侧有
   `SetVsyncEnabledScreenId:0` 日志，但 composer 侧的 `EnableVsync` 似乎
   从未被调用（worker 不运行）。建议查 RenderService 的
   `HdiDevice::InitDevice` 到 `SetScreenVsyncEnabled` 的 HDI 调用条件；
   `vsyncMode:0` 的语义也值得确认（soft 模式下 RenderService 应自行产生
   vsync 给应用，为何 ScreenLockWindow 一帧都没有？）
2. SystemUI 的 ArkUI 首帧为什么不出来：基准状态下 SystemUI 的 EGL 初始化
   失败、走 CPU 渲染（前版交接文档第 2 节）；当天没有出现失败日志，怀疑
   EGL 初始化成功后，GPU 渲染路径撞上了 1.2 节第 2 条的内存不一致问题
   （写入丢失）。可以试验：让 wrapper 的 eglGetDisplay/eglInitialize
   强制失败，迫使全系统走 CPU 渲染（即基准状态的模式）。v4 wrapper
   （out 树，md5 前缀 df998af）在
   `foundation/graphic/graphic_2d/frameworks/opengl_wrapper`，修改
   `LoadEgl` 直接 return false 后，用 `ninja … graphic/graphic_2d/libEGL.so`
   编译，部署到 /system/lib64/platformsdk/libEGL.so。
3. 与基准状态对照，唯一没能复刻的因素：基准状态下 QEMU 进程从 10-19
   20:00 启动后连续运行了数小时，期间有大量 hdc 操作。时间因素（10-19 与
   10-20）和宿主机负载无法复刻；可以尝试启动后先闲置 10 分钟再检查
   （之前的首次检查都在启动后 3–5 分钟，基准状态的取证在 5–10 分钟）。
4. faultlogger 中没有相关的崩溃记录（render_service 的 cppcrash 都是
   排查时 kill -9 产生的）。

---

## 5. 组件还原情况（做过什么，现状如何）

| 组件 | 现状（/root/ohos_disk_new.img） | 基准状态 | 是否一致 |
|------|------|------|------|
| 内核模块 virtio-gpu.ko | 3d308b53（AR24-only） | 3d308b53 | 一致 |
| composer HAL（vendor） | db200150（原始+vsync补丁） | db200150 | 一致 |
| mesa 10 库 + 软链 + v0 wrapper | 未动 | 相同 | 一致 |
| userdata | 10-18 原始（当天 dd 覆盖） | 基准日的运行状态 | 不同，但已排除影响 |
| ueventd.config | 注入行已删 | 无 | 一致 |
| OVMF_VARS | 已复位（备份：OVMF_VARS.backup.fd） | 旧 | 不同，已排除影响 |
| /data 标记 | 全部删除 | — | 一致 |

---

## 6. 关键文件索引（宿主机）

| 文件 | 说明 |
|------|------|
| /root/snap1.jpeg | 65303 色锁屏界面，前一轮的阶段成果 |
| /root/layer_dump_view.png /root/layer_now.png | composer 收到的 client buffer 内容（98k 色） |
| /root/drmprobe.c + /root/drmprobe_sysroot + /root/drmprobe_inc | DRM ioctl 探针及其编译环境 |
| /root/qemu_vg_82.c | QEMU 8.2 virtio-gpu 源码（set_scanout/transfer 分析用） |
| /root/20261018-displaynode-img/ | 10-18 组装磁盘的素材 + mkdisk2.sh 引用目录 |
| /root/ohos_disk_fresh.img | 备用盘（10-18 镜像 + 增量注入；内核模块为原版，需要 AR24 时重推） |
| /root/serial_prev*.log | 历次启动的串口日志 |
| guest /data/local/tmp/drmprobe、wrapprobe | 探针二进制 |

## 7. 注意事项（沿用前版并补充）

- Bash 环境的挂载不跨命令保留：向磁盘镜像注入文件必须在同一条命令里完成
  mount、写入、umount、losetup -d。
- 10-18 组装盘的 system 分区布局：真实目录在 `system/` 子目录下（分区根的
  lib/lib64 是指向 /system/lib、/system/lib64 的绝对软链，离开 guest 根
  目录的上下文就是断链）。
- hdc file send 偶发挂死：tconn 重连，或用 `timeout 30` 包裹命令；实在
  不行用 debugfs 直接写盘（QEMU 必须先停止）。
- 虚拟机内 reboot 后 QEMU 会退出（-no-reboot），重启 QEMU 后必须重新执行
  `hdc tconn 127.0.0.1:5555`。
- dmesg 环形缓冲会被 vsync 的 -95 周期输出快速刷掉；drm debug 每次启动都
  要重新打开。
- `power-shell timeout -o <ms>` 的覆盖值会被消耗或重置；基础 ScreenOffTime
  曾见 30000ms（30 秒灭屏）。保持亮屏需要循环刷新覆盖值
  （`while true; do hdc shell "power-shell timeout -o 3600000"; sleep 15; done`）。
  但不要用 wakeup 循环：反复触发 wakeup 可能是干扰因素之一。
