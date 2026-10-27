# 交接文档：OpenHarmony x86_general 镜像「截图非全黑」任务

> 最后更新：2026-10-20 14:15（第十五轮：**主线目标已达成**）
> 用途：交接给下一个 agent 继续推进

---

## ⭐ 核心成果（2026-10-20 14:00 达成）

**`snapshot_display` 截图 = 65,000+ 色 OHOS 蓝色锁屏 UI（mean RGB [120,159,203]），稳定且可复现。**
证据：/root/SUCCESS_SNAPSHOT.jpeg、/root/f1.jpeg、/root/f3.jpeg（三次独立冷启动 65889/65668/65247 色）

### 胜利公式（已验证 3 次，完全确定性）

1. 磁盘上已部署：wrapper libEGL.so = **70e0e669**（v4 基座 + marker 开关的 eglInitialize 强制失败）+ marker `/data/egl_no_init` + composer vendor lib = 50497250（mirror 版，marker `/data/hdi_mirror`）+ 内核 ko = 3d308b53（AR24）
2. **QEMU 冷启动**（重启 QEMU 进程，不是 guest reboot 也行——guest reboot 不换 appspawn 环境的话有崩溃循环风险）
3. 连 hdc 后 `chmod 666 /dev/fb1`（mirror 用）+ `power-shell timeout -o 3600000`（防 30 秒灭屏）
4. 等 ~3.5 分钟（SystemUI 起来）→ `snapshot_display -f /data/local/tmp/x.jpeg` → **6.5 万色**

### 根因链（今日完全闭环，推翻了此前多个假设）

- **真正的元凶：GL/软渲写入丢失**。SystemUI 走 EGL/GL 路径时「正常」渲染（有帧率、有 surface、eglQuerySurface 报错但继续），但写入 dumb-backed surface 的像素全部消失 → RS 合成空 buffer → 全黑。CPU 路径（mmap 写）像素完好 → 彩色。证据：SystemUI 稳定渲染 GL 版=黑；eglInitialize 失败版（CPU 回退）=立即彩色
- 昨天「黄金期」= SystemUI 的 EGL 恰好失败（appspawn 沙箱 HiLogPrint GOT 懒加载 bug 的温和形态）→ 自动 CPU 回退 → 彩色。今天我反复还原文件却始终黑屏，是因为该 bug 从「温和失败」翻成「崩溃循环」或「挂起」等不同形态——**形态取决于环境，但只要 EGL 成功就黑屏这一点不变**
- 修复：wrapper 里 marker 门控的 `EglWrapperDisplay::Init` 直接 return EGL_FALSE（源码 egl_wrapper_display.cpp，搜 FORCE-INIT-FAIL）→ 所有客户端确定性地走 CPU 回退；配合 v4 的 HiLogPrint stub + BlobCache 短路（否则失败路径本身会崩）
- ⚠️ **绝不要重启 render_service**：会破坏 SystemUI 的渲染态（不再恢复），唯一恢复法=再次冷启动。杀 composer_host 也大概率触发同样的停摆


---

## 0. 当前状态速览（2026-10-20 收盘）

- ✅ **原卡点（AR24 EINVAL）已修复并实锤**：内核 virtio_gpu 主平面格式表补了 ARGB8888，composer 的 drmModeAtomicCommit 全部被接受（dmesg 无 EINVAL，`handle 0xN, crtc 1280x800+0+0` 正常轮转）
- ✅ **RS→composer 内容链实锤打通**：到达 composer `SetLayerBuffer` 的 client buffer 内为 **98814 色 OHOS 蓝锁屏 UI**（多次取证，dump 文件在 /root/layer_dump_view.png 等）
- ✅ **显示最后一公里机制全部查明**（见 §2 fbcon 竞技场），mirror→fb1→console-poke→QEMU 显示通路做过 **91% 像素吻合** 的闭环验证
- ❌ **今日新回归：SystemUI 全天 0 帧渲染**（`fps ScreenLockWindow` = 0 0 0 0，snapshot 全黑，`hardwareVSyncStatus:0`）。**组件级全部还原到黄金期等价状态也无法复现渲染**（还原矩阵见 §5），判定为运行时/环境级问题，未解
- 🎁 前一轮的里程碑仍然有效：`/root/snap1.jpeg`（65303 色，mean [120,159,203]）= RS 层截图非黑的实锤

**接手第一步：直接从 §4「今日新回归」入手，那是当前唯一 blocker。**

---

## 1. 已修复并验证的问题（勿重复排查）

### 1.1 内核 AR24 + 几何 + damage（全部实锤有效）

| # | 问题 | 修复 | 验证 |
|---|------|------|------|
| 1 | virtio_gpu 主平面格式表只有 XRGB8888（XR24），OHOS client buffer 是 BGRA(=DRM AR24) → atomic commit EINVAL | `virtgpu_plane.c` 的 `virtio_gpu_formats[]` 加 `DRM_FORMAT_HOST_ARGB8888`（上游 6.6 本来就只有 XR24，属扩展而非恢复） | commit 全部接受；plane state 显示 `fb=N format=AR24 allocated by OS_IPC_1_942` |
| 2 | composer 的 atomic commit **从不带 SRC_*/CRTC_* 几何属性** → kernel 里 src/crtc 全 0x0 → scanout 零尺寸 | HAL 的 `ApplyPlane` 补 8 个几何属性（SRC_W/H 为 16.16 定点） | dmesg `handle 0xN, crtc 1280x800+0+0, src 1280x800+0+0` |
| 3 | 同 FB 无 damage 的 commit 被 virtio 驱动静默跳过（符合 DRM 语义） | `virtio_gpu_primary_plane_update` 的 damage_merged==false 分支改为全屏兜底 | QEMU trace 可见 transfer/flush 每帧到达 |

- 内核补丁在两棵树都打了：`kernel/linux/linux-6.6/`（干净树）与 `out/x86_general/kernel/src_tmp/linux-6.6/`（实际编译树）。单目标补编命令（三模块必须同一 make 调用）：
  ```
  cd out/x86_general/kernel/src_tmp/linux-6.6
  export PATH=…/prebuilts/clang/ohos/linux-x86_64/llvm/bin:…/pahole/bin:$PATH
  export DEVICE_NAME=x86_general PRODUCT_COMPANY=opc PRODUCT_PATH=vendor/opc/x86_general
  make LLVM=1 LLVM_IAS=1 KBUILD_OUTPUT=out/x86_general/kernel/OBJ/linux-6.6 \
    drivers/virtio/virtio_dma_buf.ko drivers/gpu/drm/drm_shmem_helper.ko drivers/gpu/drm/virtio/virtio-gpu.ko -j$(nproc)
  ```
- ⚠️ **ko 版本现状（重要）**：guest/镜像里的 `/system/lib/modules/virtio-gpu.ko` 现为 **3d308b53 = 仅 AR24 版**（今天回滚到黄金期等价态时换回的）。**含 damage 兜底的完整版是 058f2e3b**（源码在树里，重编即得）。AR24 是主线必需；damage 兜底是可选增强
- ⚠️ damage 兜底版曾长期部署未发现问题；回滚它只是「还原变量」实验的一部分，并非它有问题

### 1.2 显示链路的决定性事实（全部实证）

1. **QEMU screendump 就是 virtio-gpu scanout**（fb1 写 0 → 屏幕对应区域变黑验证过）
2. **用户态对 dumb buffer 的 CPU 写入（mmap dma-buf 或 MAP_DUMB 均同）与 QEMU 资源 iov 指向的内存不一致** —— drmprobe（/root/drmprobe.c，guest /data/local/tmp/drmprobe）写梯度后 SetCrtc，QEMU 收到完整 transfer/scanout/flush（trace 实证 83/80/84 条命令流）但画面不动；而内核 fbcon 自绘的内容显示正常。该「页分裂」根因未定位（理论上 attach backing 的 sgt 与 mmap 同页，全部源码核对过无 split 点），**是遗留深层问题**
3. **/dev/fb1 的 write() 不标记 deferred-io damage**（fb_io_write 纯 memcpy，源码实证）→ 用户态写 fb1 永远不会自己上屏
4. **fbcon 自身的重绘是唯一能把 fb1 内容（含用户态写入）推上屏的机制**：console 打印 → 滚动 → 全屏 damage → 推送（22:18 的 dd 全屏变黑实证 = 搭 spam 重绘的便车）
5. **composer 的 plane commit 与 fbcon 重绘互相抢 plane**：composer 每帧 commit（尤其有 damage 兜底时）会把 fbcon 刚推的内容立即覆盖成 iov 垃圾

### 1.3 mirror 方案（已验证的显示旁路，被 §4 回归阻塞）

- 位置：`device/soc/opc/x86_general/hardware/display/.../core/hdi_layer.cpp` 的 `SetLayerBuffer` 内 `MirrorToFramebuffer()`（写 /dev/fb1）+ 每帧向 /dev/console 写 "\n" 触发 fbcon 全屏推送；`hdi_drm_composition.cpp` 的 `Apply` 在 `/data/hdi_mirror` 存在时跳过 plane 循环（保留 modeset/commit/outfence）
- **⚠️ 这些改动当前在 git stash 里**（`git stash list` 可见 `my-composer-changes`），工作树=原始 HAL+前人 vsync 补丁。要用 `git stash pop` 恢复
- 曾达成：mirror 帧（SetLayerBuffer 的 UI buffer）→ fb1 → console poke → **screendump 与 fb1 内容 91% 像素吻合**
- 遗留条件：/dev/fb1 需 666（composer 用户访问，ueventd 注行方案失败过，chmod 手动可行）；console 需有输出才会推（`dmesg -n 1` 静默后必须靠 poke）

---

## 2. 「fbcon 竞技场」规则（避免再踩）

- fbcon **绑定**时：console 打印（含 init spam）会持续抢 plane；解绑后 console 输出仍走串口，fb1 冻结
- **解绑后 /dev/fb1 的 write 也不上屏**（今日多 boot 实证；只有 fbcon 在绑且活跃时写 fb1 才会被顺带推送）
- spam 源：init 的 LoopEvent timeout 警告 = 多服务 epoll 倒计时每秒打印，走 printk → **`dmesg -n 1` 一键静默**（串口+fbcon 同时安静）

---

## 3. 环境速查（沿袭前版，均有效）

| 项 | 值 |
|---|------|
| QEMU 磁盘 | **/root/ohos_disk_new.img**（AR24 ko + 黄金期等价 libs + 原始 userdata）；**/root/ohos_disk_fresh.img** = 1018 拼盘+注入的备用盘（ko 为 1018 原版 ed110b7d，其余增量已注入） |
| QEMU 启动 | 见 /root/.bash_history 或交接 §4；`-s` 可留可去（实测无影响）；`-no-reboot` 在，guest reboot 后必须手动重启 QEMU 进程 |
| ninja | `cd out/x86_general && /root/oh_env/bin/ninja -w dupbuild=warn <target>`；composer HAL 目标：`opc_soc_products/x86_general/libdisplay_composer_vendor.z.so` |
| drm debug | guest `echo 31 > /sys/module/drm/parameters/debug`；-95 心跳 = 软 vsync worker 在跑（**注意每个新 boot 都要重新开 debug，且 dmesg 环形缓冲会被刷掉**） |
| QEMU trace | 启动加 `-trace enable='virtio_gpu*'`，日志在 qemu_stdout.log（事件名：res_xfer_toh_2d / set_scanout / res_flush / res_create_2d / res_back_attach） |
| 探针 | /root/drmprobe.c（raw ioctl DRM 探针，编译：clang --target=x86_64-linux-ohos --sysroot=/root/drmprobe_sysroot -I/root/drmprobe_inc）；/root/wrapprobe（EGL/GL，今日全通=`GL栈健康`） |

## 4. 今日新回归：SystemUI 0 帧（当前唯一 blocker）

**症状（每个 boot 稳定复现）**：
- `snapshot_display` = 纯黑（1 色）；`hidumper -s RenderService -a 'fps ScreenLockWindow'` = 0 0 0 0
- `hidumper -s RenderService -a allInfo` → **`hardwareVSyncStatus:0`**（黄金期为 1）
- SystemUI 进程活着（TIME_TICK/控制中心日志正常），窗口树里有 ScreenLockWindow（1280x800, z=4），但窗口 surface 从未 flush 过一帧
- composer 的 vsync worker（-95 spam）**在 boot 后不跑**；杀 composer 重启后 worker 恢复跑，但 RS 的 sampler 仍 0 → 回调链没接上
- RS init 日志正常（RegRefreshCallback/-2 等错误与黄金期相同）；`power-shell dump -s` = AWAKE；DisplayPowerManager State=2（DIM）

**已排除（全部试过，无效）**：
- 回滚内核 ko 到 AR24-only（3d308b53=黄金期同版本）✗
- 回滚 composer HAL 到原始（git stash，db200150=黄金期同 md5）✗
- 恢复 1018 原始 userdata（dd 分区镜像）✗
- 删除 ueventd 注入行、复位 OVMF_VARS、QEMU 加回 -s、字节级复刻启动参数 ✗
- 全新拼盘（mkdisk2.sh + 1018 + 注入全部黄金期增量：mesa10库/软链5/v0 wrapper/vendor impl4）✗
- 各种杀服务顺序（composer→RS、RS→composer、同杀、SystemUI 单杀）✗
- GL 栈健康（wrapprobe 全通）✓ 排除 mesa 部署问题

**未排除的方向（接手建议）**：
1. **RS 的 vsync 使能链**：RS 侧 `SetVsyncEnabledScreenId:0` 有日志，但 composer 侧 `EnableVsync` 似乎从未被调（worker 不跑）→ 查 RS 的 `HdiDevice::InitDevice` → `SetScreenVsyncEnabled` 的 HDI 调用条件；`vsyncMode:0` 的语义（soft 模式下 RS 应自产 vsync 给 app，为何 ScreenLockWindow 一帧都没有？）
2. **SystemUI 的 ArkUI 首帧为何不出**：黄金期 SystemUI EGL 失败→CPU 渲染（前版交接 §2）；今日无失败日志 → 怀疑 EGL 成功后 GPU 渲染路径撞上 §1.2-2 的页分裂 bug（写入消失）→ 可试验：让 wrapper 的 eglGetDisplay/eglInitialize 强制失败，逼全系统 CPU 渲染（黄金期模式）。v4 wrapper（out 树, md5 df998af）在 `foundation/graphic/graphic_2d/frameworks/opengl_wrapper`，可改 `LoadEgl` 直接 return false 再 `ninja … graphic/graphic_2d/libEGL.so` 部署 /system/lib64/platformsdk/libEGL.so
3. **对照黄金期唯一没复刻的**：黄金期 QEMU 进程从 10/19 20:00 host 起连续跑了数小时且中间经历了大量 hdc 活动。时间因素（10/19 vs 10/20）与 host 负载无法复刻；但可以试「boot 后闲置 10 分钟再查」（首查都在 +3~5 分钟，黄金期取证在 +5~10 分钟）
4. faultlogger 无相关 crash（render_service 的 cppcrash 都是我 kill -9 的）

---

## 5. 还原矩阵（做过什么，现状如何）

| 组件 | 现状（/root/ohos_disk_new.img） | 黄金期 | 一致? |
|------|------|------|------|
| 内核 ko virtio-gpu.ko | 3d308b53（AR24-only） | 3d308b53 | ✅ |
| composer HAL（vendor） | db200150（原始+vsync补丁） | db200150 | ✅ |
| mesa 10 库 + 软链 + v0 wrapper | 未动 | 同 | ✅ |
| userdata | **1018 原始**（今日 dd 覆盖） | 昨日运行态 | ⚠️ 不同但已排除 |
| ueventd.config | 注入行已删 | 无 | ✅ |
| OVMF_VARS | 已复位（backup: OVMF_VARS.backup.fd） | 旧 | ⚠️ 已排除 |
| /data 标记 | 全删 | - | ✅ |

---

## 6. 关键文件索引（host）

| 文件 | 说明 |
|------|------|
| /root/snap1.jpeg | **65303 色锁屏 UI —— 主线阶段性成果实锤（前一轮）** |
| /root/layer_dump_view.png /root/layer_now.png | composer 收到的 client buffer 内容（98k 色实证） |
| /root/drmprobe.c + /root/drmprobe_sysroot + /root/drmprobe_inc | DRM raw ioctl 探针及编译环境 |
| /root/qemu_vg_82.c | QEMU 8.2 virtio-gpu 源码（set_scanout/transfer 分析用） |
| /root/20261018-displaynode-img/ | 1018 拼盘素材 + mkdisk2.sh 引用目录 |
| /root/ohos_disk_fresh.img | 备用盘（1018+增量注入；ko 为原版，要用 AR24 需重推） |
| /root/serial_prev*.log | 历次 boot 串口日志 |
| guest /data/local/tmp/drmprobe、wrapprobe | 探针二进制 |

## 7. 注意事项（沿袭+新增）

- Bash 沙箱挂载不跨调用：**disk 镜像注入必须在同一条命令里 mount→写→umount→losetup -d**
- 1018 拼盘的 system 分区布局：真目录在 `system/` 子目录下（分区根的 lib/lib64 是指向 /system/lib[64] 的绝对软链，脱离 guest 根上下文是断链）
- hdc file send 偶发挂死：tconn 重连或换 `timeout 30` 包裹；实在不行用 debugfs 直接写盘（QEMU 必须先停）
- guest reboot 后 QEMU 会退出（-no-reboot），重启 QEMU 后**必须 hdc tconn 127.0.0.1:5555**
- dmesg 环形缓冲被 vsync -95 spam 高速刷掉；drm debug 每个 boot 都要重新开
- `power-shell timeout -o <ms>` 的 override 会被消耗/重置；基础 ScreenOffTime 曾见 30000ms（30 秒灭屏），保亮屏需循环刷新 override（`while true; do hdc shell "power-shell timeout -o 3600000"; sleep 15; done`）——**但别用 wakeup 循环**（wakeup 反复触发可能是干扰项之一）
