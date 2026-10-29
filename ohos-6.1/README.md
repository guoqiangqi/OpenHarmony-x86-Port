# OpenHarmony 6.1 (API 23) x86_general 移植构建指南

> 状态（2026-10-29）：功能已与 5.1 交付版对齐。`hdc snapshot_display` 全像素
> 统计 66,027 色，mean RGB [120,159,203]（同一环境下 5.1 的结果是 65,675 色，
> 锁屏界面相同）；`param get const.ohos.apiversion` 返回 23。组装磁盘后必须
> 执行 `scripts/fixup-disk.sh`（合并 vendor、把 data 分区重新格式化、设置 GL
> 环境变量），原因见第 8 节。

## 1. 源码获取

OpenHarmony 主干源码用 repo 工具按 tag 同步（不适合 submodule 管理）：

```bash
mkdir /root/OpenHarmony/6.1-Release && cd /root/OpenHarmony/6.1-Release
repo init -u https://gitee.com/openharmony/manifest.git \
    -b refs/tags/OpenHarmony-v6.1-Release --no-repo-verify
repo sync -c -j8 --no-tags --optimized-fetch
repo forall -c "git lfs install --skip-repo 2>/dev/null; git lfs pull"
```

- repo 启动器可以用清华镜像：`https://mirrors.tuna.tsinghua.edu.cn/git/git-repo`
- git-lfs 必须执行，否则 libphonenumber 等 LFS 文件只是指针，编译时报
  `unknown type name 'version'`。

### 1.1 opc 三个仓库（x86 硬件适配层）

社区的 opc 三个仓库最高只到 6.0-Beta1，6.1 的适配是自行移植的，基线如下：

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

## 2. 内核：整棵换用 5.1 源码树的 linux-6.6

6.1 manifest 自带的 linux-6.6（6.6.23）编译不出 x86_64 的 bzImage：kvm/ksm/tls
相关补丁只合并了一部分（`kvm_iodevice_destructor` 未声明、folio_walk API
残缺），`amd-fch.h` 整个文件缺失。逐个修复的成本高于换树，因此直接改用
5.1 源码树的 linux-6.6（6.6.86，已包含 AR24 显示修复和 4 个内核模块的单
目标构建方案）：

```bash
rm -rf kernel/linux/linux-6.6
cp -a <已打好补丁的 5.1 树>/kernel/linux/linux-6.6 kernel/linux/linux-6.6
rm -rf out/x86_general/kernel/src_tmp out/x86_general/kernel/OBJ   # 强制重拷
```

注意：5.1 源码树必须已应用 ohos-5.1/patches/01（virtio-gpu AR24）和
06（accesstokenid 双重释放）。其中 06 修复的是上游 linux-6.6 的内核缺陷，
不打这个补丁，6.1 的用户态会在开机约 557 秒时触发
`kernel BUG at mm/slub.c:448`（见第 8 节问题 1）。vermagic 的差异（6.6.86）
由 build_kernel.sh 的单目标联合编译保证模块匹配。

## 3. 打补丁

```bash
bash scripts/apply-x86-port-patches.sh /root/OpenHarmony/6.1-Release
```

脚本依次执行：opc 自带的 patch.sh（允许 fuzz）、恢复 musl 上游版的
namespace ini、ohos-repos 补丁集（11–25）、opc-trio 增量补丁（01–04），
最后拷入 untracked 目录下的新增文件。

## 4. SDK 组装（编译 hap 需要）

公开 SDK 缺少 systemapi 的类型声明，源码构建的 SDK 缺少宿主机侧的工具链，
因此需要把两者组合使用，详见 `scripts/setup-sdk.sh` 的头部注释：

```bash
# 4.1 公开 SDK（宿主机侧 ets/js/previewer/toolchains 二进制）
wget https://repo.huaweicloud.com/openharmony/os/6.1-Release/ohos-sdk-windows_linux-public.tar.gz
# 4.2 源码 SDK（systemapi 类型声明 + native sysroot）
source <python3.10 venv>/bin/activate && ./build.sh --product-name ohos-sdk --ccache
# 4.3 组合
bash scripts/setup-sdk.sh /root/OpenHarmony/6.1-Release <下载的tar.gz>
```

## 5. 构建

```bash
apt-get install -y autoconf automake libtool nasm autoconf-archive   # libnl/libvpx 需要
source /root/oh_env61/bin/activate        # python3.10 虚拟环境
./build.sh --product-name x86_general --ccache
```

注意事项：

- Ubuntu 24.04（glibc 2.39）需要给宿主机编译的 cflags_cc 加 `-include string`
  （补丁 11 已包含）。
- system.img 的默认配额 2G 不够（6.1 的 system 内容约 2.2G，e2fsdroid 阶段
  报 `Could not allocate block`）。补丁集已把 `system_image_conf.txt` 提高
  到 4G，组装磁盘时也使用 4G 分区。
- 构建高峰期磁盘占用较高（out 目录约 98G），建议通过 `ccache -M 8G` 限制
  缓存大小。

## 6. 组装磁盘与启动

```bash
cd <staging>   # 放 bzImage/ramdisk/system/vendor/userdata.img 与 scripts/disk_assemble.sh
cp <tree>/out/x86_general/packages/phone/images/{bzImage,ramdisk.img,system.img,vendor.img,userdata.img} .
bash scripts/disk_assemble.sh ./ohos_disk_61.img
bash scripts/fixup-disk.sh ./ohos_disk_61.img <tree>/out/x86_general   # 必须执行
bash ../common/launch_qemu.sh ./ohos_disk_61.img     # grub: root=PARTLABEL=system
```

`fixup-disk.sh` 做四件事（分别对应第 8 节的问题 3、4、5 和第 9 节的 GL
策略）：把 vendor 分区内容合并进 system 分区根部的 /vendor；把 data 分区
重新格式化为 ext4；拷入 hdf_devhost；设置 GL 环境变量并补充 DRI 驱动别名。

## 7. 图形栈：mesa 换用 5.1 修复版

6.1 镜像预置的 mesa 还是旧的 emutls 版本，会导致 SystemUI 和 appspawn 崩溃
循环。需要换成 5.1 的修复版（`tools/mesa-libs/`，即用 `-fno-emulated-tls`
重新构建的产物，构建方法见 ohos-5.1/patches/05 与 apply-emutls-flag.sh）：

- 源码树侧：替换 `device/soc/opc/x86_general/hardware/gpu/lib64/` 下的
  同名 so。
- 已经组好的镜像侧（应急做法）：替换 `/system/lib64/` 下的同名 so。6.1 的
  布局是所有库放在同一目录，`lib*_impl.so` 由上游 BUILD.gn 以符号链接方式
  生成，不需要单独放置。

替换后，每次启动 appspawn 的内核崩溃转储从 118 次降到 0。

## 8. 移植过程中解决的问题

| # | 问题 | 根因与修复 |
|---|------|-----------|
| 1 | 开机约 557s `kernel BUG at mm/slub.c:448` panic | 上游 accesstokenid 驱动删根节点时，把替换指针写进按值传入的局部参数地址，`g_token_perm_root` 悬垂引发双重释放。修复见 ohos-5.1/patches/06（kernel） |
| 2 | foundation 每 15 秒 SEGV 一次 | libapp_domain_verify 静态初始化调用 RestoreconRecurse，在无 file_contexts 时 selabel_open 的失败清理路径 free 了未初始化指针。修复见补丁 25（无路径选项时跳过 selabel_open，selinux_adapter） |
| 3 | HDF host 全部起不来（composer_host 等），multimodalinput 冻结 | 6.1 运行时不挂载 vendor 分区：init 在挂载任何分区之前就要读 `/vendor/etc/fstab.x86_general`，而 6.1 把 vendor 内容拆到了独立镜像，fstab 因此读不到。修复：组装磁盘时把 vendor 分区内容合并进 system 分区根部的 /vendor（fixup-disk.sh） |
| 4 | 根文件系统只读、/data 写入失败 | fstab 可读之后，对 f2fs 格式的 data 分区执行 ext4 的 resize2fs/e2fsck 失败（f2fs.ko 未构建）。修复：把 data 分区直接重新格式化为 ext4（fixup-disk.sh） |
| 5 | `pathArgs invalid: /vendor/bin/hdf_devhost` | 上游把 hdf_devhost 从 hdf_core 拆到 drivers_adapter 组件，产品配置未包含。修复：在 vendor/opc 的 config.json 里给 hdf 子系统加入 drivers_adapter（opc-trio 补丁 01），fixup-disk.sh 里另有兜底拷贝 |
| 6 | hdc（SDK23）握手失败 | 问题 2、3 的连带症状，foundation 与 composer_host 恢复正常后随之好转 |
| 7 | 应用侧 GL 无法初始化，SystemUI 帧请求全部失败 | mesa 找不到 kms_swrast DRI 驱动：`LIBGL_DRIVERS_PATH` 只给了 render_service。修复：graphic.cfg 的 init job 全局 export `LIBGL_ALWAYS_SOFTWARE=1` 与 `LIBGL_DRIVERS_PATH=/system/lib64`（init 的 export 会传给它拉起的全部服务），并补 kms_swrast/swrast/virtio_gpu_dri 别名（fixup-disk.sh） |
| 8 | RenderContextGL 三处缺陷导致客户端 EGL 上下文初始化失败 | 与 5.1 补丁 02 修复过的旧文件同源，在 6.1 的新文件里再次出现：eglQueryString(NO_DISPLAY) 返回 nullptr 时跳过 pbuffer 创建，导致 surfaceless 初始化失败；ES3 的 config 配了 v2 的 context；RSSurfaceOhosGl::RequestFrame 首次因竞态失败后被永久缓存。修复见补丁 21（graphic_2d） |
| 9 | RenderService 收不到 vsync，渲染循环不启动 | HAL 的 drm_vsync_worker 生命周期缺陷：mRunning 在线程启动后才置位（竞态可直接退出线程），且线程等待的 EnableVsync 信号不会到来。修复：先置位 mRunning，软 vsync 模式下自由运行（opc-trio 补丁 04，device/soc/opc） |

## 9. 验证方法与注意项

截图验证的方法（两个版本相同）：

```bash
hdc shell snapshot_display -f /data/local/tmp/x.jpeg && hdc file recv /data/local/tmp/x.jpeg .
# 全像素统计应 >= 60000 色，mean RGB ≈ [120,159,203]（锁屏界面）
bash ../common/verify_snapshot.sh out.jpeg --sdk23
```

- 不要用 QEMU monitor 的 screendump 判断画面内容：guest 里的 composer 并不
  驱动 QEMU 的扫描输出，5.1 和 6.1 的成品盘在 QEMU 下都只显示灰色噪声
  残影。
- 不要在 6.1 上使用 5.1 的 `/data/egl_no_init` 机制：6.1 的 RenderService
  和 ArkUI 没有 CPU 渲染回退，强制 eglInitialize 失败会导致一帧都渲染不出
  来。两个版本的图形策略相反（5.1 是标记文件加 CPU 渲染，6.1 要求 GL
  完整走通），见第 10 节。
- 排查手段：RenderService 属于系统进程，可以写 /dev/kmsg 做探针；应用
  沙箱进程打不开 /dev/kmsg，也看不到宿主环境的 /data/local/tmp（挂载
  命名空间隔离）。
- 5.1（SDK linux/18）与 6.1（SDK linux/23）的 hdc 不能共用一个守护进程，
  切换时先 `hdc kill`。
- 给镜像换库时注意 system/lib64 与 system/lib64/platformsdk 可能各有一份
  副本，应用进程加载的是 platformsdk 下的那份。

## 10. 6.1 与 5.1 的关键区别

| 项 | 5.1 | 6.1 |
|----|-----|-----|
| 图形策略 | `/data/egl_no_init` 标记文件强制 eglInitialize 失败，走 CPU 渲染 | GL 完整初始化（全局 LIBGL 环境变量）；标记文件会导致 0 帧，禁用 |
| 产品 config 组件键名 | `"name"` | `"component"` |
| opengl wrapper | wrapper 与 impl 分层（impl 在 vendor/lib64/chipsetsdk） | wrapper 在 platformsdk/libEGL.so，impl 直接放在 /system/lib64（符号链接） |
| vendor 内容 | 内嵌在 system 镜像根部的 /vendor | 拆到独立 vendor.img，组装磁盘时需合并回 system 分区 |
| data 分区 | ext4 | 构建产物为 f2fs（且 f2fs.ko 未构建），需重新格式化为 ext4 |
| build_selinux | false 即可启动 | 必须为 false（true 时 init param service 被 AVC 卡死 panic） |
| musl 加载器 ini | 5.1 定制版 | 必须用 6.1 上游原版（含 chipset-sdk-sp 与 /system/bin 场景；用 5.1 版会 init 静默 `_exit(127)`） |
| 内核 | 自带 6.6.86 可用 | 自带 6.6.23 不可用，整棵换用 5.1 内核 |
| hdf_devhost | hdf_core 直接构建 | 拆到 drivers_adapter 组件，需加入产品配置 |
| system.img 配额 | 2016M | 4096M |
| Node | 14.21.1 | 18.20.1（ets2panda 依赖 node:diagnostics_channel） |
| mindspore/nnrt | 可构建 | 树内 nnacl 引用的文件不存在，已从产品配置移除（相关 AI 组件不可用） |
