# OpenHarmony 6.1 (API 23) x86_general 移植构建指南

> 状态（2026-10-29）：功能与 5.1 交付版对齐。`hdc snapshot_display` 全像素统计
> 66,027 色 / mean RGB [120,159,203]（5.1 同机基准 65,675 色，同一锁屏 UI）；
> `param get const.ohos.apiversion` = 23。拼盘后必须执行
> `scripts/fixup-disk.sh`（vendor 合并、data 分区重格、GL 环境变量），原因见 §8。

## 1. 源码获取

OpenHarmony 本体用 repo 工具按 tag 同步（不适用 submodule）：

```bash
mkdir /root/OpenHarmony/6.1-Release && cd /root/OpenHarmony/6.1-Release
repo init -u https://gitee.com/openharmony/manifest.git \
    -b refs/tags/OpenHarmony-v6.1-Release --no-repo-verify
repo sync -c -j8 --no-tags --optimized-fetch
repo forall -c "git lfs install --skip-repo 2>/dev/null; git lfs pull"
```

- repo 启动器可用清华镜像：`https://mirrors.tuna.tsinghua.edu.cn/git/git-repo`
- git-lfs 必须执行，否则 libphonenumber 等 LFS 文件是指针，编译报
  `unknown type name 'version'`。

### 1.1 opc 三仓（x86 硬件层）

社区 opc 三仓最高只到 6.0-Beta1，6.1 属自行移植，基线：

| 仓库 | 上游（gitee.com/diemit） | 基线 commit |
|------|--------------------------|-------------|
| vendor_opc | vendor_opc.git @OpenHarmony-6.0-Beta1 | 55d1ec2687bb4ff162ab62aa70509cd90fb533c4 |
| device_board_opc | device_board_opc.git @OpenHarmony-6.0-Beta1 | 3b57fcfc551a5df4ab8b6f1dbf6c3e903b0e1b93 |
| device_soc_opc | device_soc_opc.git @OpenHarmony-6.0-Beta1 | 6f7235d3c113e278b99dc346137d757e5ffc8025 |

```bash
git clone https://gitee.com/diemit/vendor_opc.git vendor/opc
git -C vendor/opc checkout 55d1ec2687bb4ff162ab62aa70509cd90fb533c4
# device/board/opc、device/soc/opc 同理
```

## 2. 内核：整树换用 5.1 树的 linux-6.6

6.1 manifest 自带的 linux-6.6（6.6.23）编不出 x86_64 bzImage：kvm/ksm/tls
补丁半合并（`kvm_iodevice_destructor` 未声明、folio_walk API 残缺）、
`amd-fch.h` 整文件缺失。修复成本高于换树，因此直接用 5.1 树的 linux-6.6
（6.6.86，已含 AR24 显示修复与 4 个 ko 的单目标构建方案）：

```bash
rm -rf kernel/linux/linux-6.6
cp -a <已打好补丁的 5.1 树>/kernel/linux/linux-6.6 kernel/linux/linux-6.6
rm -rf out/x86_general/kernel/src_tmp out/x86_general/kernel/OBJ   # 强制重拷
```

注意：5.1 树必须已应用 ohos-5.1/patches/01（virtio-gpu AR24）和
06（accesstokenid 双重释放）。其中 06 修复上游 linux-6.6 的内核 bug，
不打则 6.1 用户态会在开机约 557 秒时触发 `kernel BUG at mm/slub.c:448`
（问题清单 §8 第 1 条）。vermagic 差异（6.6.86）由 build_kernel.sh
单目标联编保证 ko 匹配。

## 3. 打补丁

```bash
bash scripts/apply-x86-port-patches.sh /root/OpenHarmony/6.1-Release
```

脚本依次执行：opc 自带 patch.sh（允许 fuzz）→ 恢复 musl 上游版
namespace ini → ohos-repos 补丁集（11–25）→ opc-trio 增量补丁（01–04）→
拷入 untracked 新文件。

## 4. SDK 组装（编译 hap 需要）

公开 SDK 缺 systemapi typings，源码构建的 SDK 缺 host 侧工具链，需要混装，
详见 `scripts/setup-sdk.sh` 头注释：

```bash
# 4.1 公开 SDK（host 侧 ets/js/previewer/toolchains 二进制）
wget https://repo.huaweicloud.com/openharmony/os/6.1-Release/ohos-sdk-windows_linux-public.tar.gz
# 4.2 源码 SDK（systemapi typings + native sysroot）
source <python3.10 venv>/bin/activate && ./build.sh --product-name ohos-sdk --ccache
# 4.3 混装
bash scripts/setup-sdk.sh /root/OpenHarmony/6.1-Release <下载的tar.gz>
```

## 5. 构建

```bash
apt-get install -y autoconf automake libtool nasm autoconf-archive   # libnl/libvpx 需要
source /root/oh_env61/bin/activate        # python3.10 venv
./build.sh --product-name x86_general --ccache
```

注意事项：

- Ubuntu 24.04（glibc 2.39）需给 host 编译 cflags_cc 加 `-include string`
  （补丁 11 已包含）。
- system.img 配额默认 2G 不够（6.1 system 内容约 2.2G，e2fsdroid 阶段报
  `Could not allocate block`），补丁集已把 `system_image_conf.txt` 提到 4G；
  拼盘也用 4G 分区。
- 构建峰值磁盘占用较高（out 约 98G），建议定期 `ccache -M 8G`。

## 6. 拼盘与启动

```bash
cd <staging>   # 放 bzImage/ramdisk/system/vendor/userdata.img 与 scripts/disk_assemble.sh
cp <tree>/out/x86_general/packages/phone/images/{bzImage,ramdisk.img,system.img,vendor.img,userdata.img} .
bash scripts/disk_assemble.sh ./ohos_disk_61.img
bash scripts/fixup-disk.sh ./ohos_disk_61.img <tree>/out/x86_general   # 必须执行
bash ../common/launch_qemu.sh ./ohos_disk_61.img     # grub: root=PARTLABEL=system
```

`fixup-disk.sh` 做四件事（对应 §8 问题 3、4、5 与 §9 的 GL 策略）：
把 vendor 分区内容合并进 system 分区根的 /vendor、把 data 分区重格为
ext4、拷入 hdf_devhost、设置 GL 环境变量并补 DRI 驱动别名。

## 7. 图形栈：mesa 换用 5.1 修复版

6.1 镜像预置的 mesa 是旧 emutls 版，会导致 SystemUI/appspawn 崩溃循环。
用 5.1 修复版替换（`tools/mesa-libs/`，即 `-fno-emulated-tls` 重建产物，
构建方法见 ohos-5.1/patches/05 与 apply-emutls-flag.sh）：

- 源码树侧：替换 `device/soc/opc/x86_general/hardware/gpu/lib64/` 下同名 so。
- 已拼好的镜像侧（应急）：替换 `/system/lib64/` 下同名 so。6.1 布局为平铺，
  `lib*_impl.so` 由上游 BUILD.gn 以符号链接方式生成，无需单独放置。

替换后 appspawn 的内核崩溃 dump 由 118 次/启动降为 0。

## 8. 移植过程中解决的问题

| # | 问题 | 根因与修复 |
|---|------|-----------|
| 1 | 开机约 557s `kernel BUG at mm/slub.c:448` panic | 上游 accesstokenid 驱动删根节点时把替换指针写进按值传入的局部参数地址，`g_token_perm_root` 悬垂引发双重释放。修复见 ohos-5.1/patches/06（kernel） |
| 2 | foundation 每 15 秒 SEGV 一次 | libapp_domain_verify 静态初始化调用 RestoreconRecurse，在无 file_contexts 时 selabel_open 的失败清理路径 free 未初始化指针。修复见补丁 25（无路径选项时跳过 selabel_open，selinux_adapter） |
| 3 | HDF host 全部起不来（composer_host 等），multimodalinput 冻结 | 6.1 运行时不挂载 vendor 分区：init 在挂载任何分区前读 `/vendor/etc/fstab.x86_general`，而 6.1 把 vendor 内容拆到了独立镜像导致 fstab 不可达。修复：拼盘时把 vendor 分区内容合并进 system 分区根 /vendor（fixup-disk.sh） |
| 4 | 根文件系统只读、/data 写入失败 | fstab 可达后按 ext4 对 f2fs 的 data 分区跑 resize2fs/e2fsck 失败（f2fs.ko 未构建）。修复：data 分区直接重格为 ext4（fixup-disk.sh） |
| 5 | `pathArgs invalid: /vendor/bin/hdf_devhost` | 上游把 hdf_devhost 从 hdf_core 拆到 drivers_adapter 组件，产品配置未包含。修复：vendor/opc config.json 的 hdf 子系统加入 drivers_adapter（opc-trio 补丁 01），另在 fixup-disk.sh 中兜底拷贝 |
| 6 | hdc（SDK23）握手失败 | 问题 2、3 的连带症状，foundation 与 composer_host 正常后自愈 |
| 7 | 应用侧 GL 无法初始化，SystemUI 帧请求全部失败 | mesa 找不到 kms_swrast DRI 驱动：`LIBGL_DRIVERS_PATH` 只给了 render_service。修复：graphic.cfg 的 init job 全局 export `LIBGL_ALWAYS_SOFTWARE=1` 与 `LIBGL_DRIVERS_PATH=/system/lib64`（init 的 export 会传给它拉起的全部服务），并补 kms_swrast/swrast/virtio_gpu_dri 别名（fixup-disk.sh） |
| 8 | RenderContextGL 三处缺陷导致客户端 EGL 上下文初始化失败 | 与 5.1 patch 02 修的老文件同源、在 6.1 的新文件里复发：eglQueryString(NO_DISPLAY) 返回 nullptr 时跳过 pbuffer 创建导致 surfaceless 失败；ES3 config 配了 v2 context；RSSurfaceOhosGl::RequestFrame 首次竞态失败后被永久缓存。修复见补丁 21（graphic_2d） |
| 9 | RS 收不到 vsync，渲染循环不启动 | HAL 的 drm_vsync_worker 生命周期缺陷：mRunning 在线程启动后才置位（竞态可直接退出线程），且等待的 EnableVsync 门控不会到来。修复：先置位 mRunning，软 vsync 模式下自由运行（opc-trio 补丁 04，device/soc/opc） |

## 9. 验证方法与注意项

验证截图的口径（两版一致）：

```bash
hdc shell snapshot_display -f /data/local/tmp/x.jpeg && hdc file recv /data/local/tmp/x.jpeg .
# 全像素统计应 >= 60000 色，mean RGB ≈ [120,159,203]（锁屏 UI）
bash ../common/verify_snapshot.sh out.jpeg --sdk23
```

- QEMU monitor 的 screendump 不能作为内容判据：guest 的 composer 不驱动
  QEMU 扫描输出，5.1 与 6.1 的成品盘在 QEMU 下都只显示灰噪残影。
- 不要在 6.1 上使用 5.1 的 `/data/egl_no_init` 机制：6.1 的 RS 与 ArkUI 没有
  CPU 渲染回退，强制 eglInitialize 失败会导致 0 帧。两版的图形策略相反
  （5.1 = marker + CPU 渲染；6.1 = GL 全通），见 §10。
- 排查手段：RS（系统进程）可写 /dev/kmsg 做探针；app 沙箱进程打不开
  /dev/kmsg，也看不到宿主的 /data/local/tmp（挂载命名空间隔离）。
- 5.1（SDK linux/18）与 6.1（SDK linux/23）的 hdc 不能共用一个守护进程，
  切换时先 `hdc kill`。
- 给镜像换库时注意 system/lib64 与 system/lib64/platformsdk 可能各有一份
  副本，应用进程加载的是 platformsdk 下的那份。

## 10. 6.1 与 5.1 的关键区别

| 项 | 5.1 | 6.1 |
|----|-----|-----|
| 图形策略 | `/data/egl_no_init` marker 强制 eglInitialize 失败，走 CPU 渲染 | 让 GL 完整初始化（全局 LIBGL 环境变量）；marker 会导致 0 帧，禁用 |
| 产品 config 组件键名 | `"name"` | `"component"` |
| opengl wrapper | wrapper + impl 分层（impl 在 vendor/lib64/chipsetsdk） | wrapper 在 platformsdk/libEGL.so，impl 平铺 /system/lib64（符号链接） |
| vendor 内容 | 内嵌在 system 镜像根 /vendor | 拆到独立 vendor.img，需拼盘时合并回 system 分区 |
| data 分区 | ext4 | 构建产物为 f2fs（且 f2fs.ko 未构建），需重格为 ext4 |
| build_selinux | false 可启动 | 必须为 false（true 时 init param service 被 AVC 卡死 panic） |
| musl 加载器 ini | 5.1 定制版 | 必须用 6.1 上游原版（含 chipset-sdk-sp 与 /system/bin 场景；用 5.1 版会 init 静默 `_exit(127)`） |
| 内核 | 自带 6.6.86 可用 | 自带 6.6.23 不可用，整树换 5.1 内核 |
| hdf_devhost | hdf_core 直接构建 | 拆到 drivers_adapter 组件，需加入产品配置 |
| system.img 配额 | 2016M | 4096M |
| Node | 14.21.1 | 18.20.1（ets2panda 依赖 node:diagnostics_channel） |
| mindspore/nnrt | 可构建 | 树内 nnacl 引用的文件不存在，已从产品配置移除（AI 栈不可用） |
