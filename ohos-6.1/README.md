# OpenHarmony 6.1 (API 23) x86_general 移植构建指南

> 状态（2026-10-28）：镜像可从源码完整构建并启动到用户态；内核 slub BUG、foundation
> selabel 崩溃循环、vendor/data 分区不挂载（composer/mmi 全灭根因）、f2fs/ext4 错配、
> hdf_devhost 缺失、hdc 握手 —— **全部已解决**（§8）；GL 已验证 `eglInitialize OK`。
> 拼盘后必须跑 `scripts/fixup-disk.sh`（vendor 合并 + data 重格 + GL 使能）。
> 唯一卡点：**RS 渲染循环 0 帧**（§8b）。详见文末。

## 1. 源码获取（OHOS 本体不走 submodule，用 repo 工具）

```bash
mkdir /root/OpenHarmony/6.1-Release && cd /root/OpenHarmony/6.1-Release
repo init -u https://gitee.com/openharmony/manifest.git \
    -b refs/tags/OpenHarmony-v6.1-Release --no-repo-verify
repo sync -c -j8 --no-tags --optimized-fetch
repo forall -c "git lfs install --skip-repo 2>/dev/null; git lfs pull"   # 必做！
```

- repo 启动器：`https://mirrors.tuna.tsinghua.edu.cn/git/git-repo`（gitee 限流时的镜像）
- **git-lfs 必须拉取**，否则 libphonenumber/geocoding_data.cc 等是 LFS 指针，编译报
  `unknown type name 'version'`。

### 1.1 opc 三仓（x86 硬件移植层，社区最高只到 6.0-Beta1 → 属自行移植）

| 仓库 | 上游 | 6.1 基线 commit |
|------|------|-----------------|
| vendor_opc | https://gitee.com/diemit/vendor_opc.git @OpenHarmony-6.0-Beta1 | 55d1ec2687bb4ff162ab62aa70509cd90fb533c4 |
| device_board_opc | https://gitee.com/diemit/device_board_opc.git @OpenHarmony-6.0-Beta1 | 3b57fcfc551a5df4ab8b6f1dbf6c3e903b0e1b93 |
| device_soc_opc | https://gitee.com/diemit/device_soc_opc.git @OpenHarmony-6.0-Beta1 | 6f7235d3c113e278b99dc346137d757e5ffc8025 |

```bash
git clone https://gitee.com/diemit/vendor_opc.git vendor/opc
git -C vendor/opc checkout 55d1ec2687bb4ff162ab62aa70509cd90fb533c4
# device/board/opc、device/soc/opc 同理
```

## 2. 内核：直接使用 5.1 树的 linux-6.6（关键决策）

6.1 manifest 自带 linux-6.6（6.6.23）**编不出 x86_64 bzImage**：kvm/ksm/tls 半合并
（`kvm_iodevice_destructor` 未声明、folio_walk API 残缺）、`amd-fch.h` 整文件缺失。
修复成本远高于换树，故整树替换为 **5.1 树的 linux-6.6（6.6.86，已含 AR24 显示修复
与 4-ko 单目标构建方案）**：

```bash
rm -rf kernel/linux/linux-6.6
cp -a <已打好 5.1 补丁的树>/kernel/linux/linux-6.6 kernel/linux/linux-6.6   # 必须含 01 + 06 两个内核补丁
rm -rf out/x86_general/kernel/src_tmp out/x86_general/kernel/OBJ   # 强制重拷
```

其中补丁 06（accesstokenid 双重释放根修）是 6.1 能存活过开机 ~557s 的**必要条件**
（上游 linux-6.6 自带 bug，详见 ohos-5.1/patches/06 与 §8.1）。

版本号差异（vermagic 6.6.86）由 build_kernel.sh 单目标联编保证 ko 匹配，无碍。

## 3. 打补丁

```bash
bash scripts/apply-x86-port-patches.sh /root/OpenHarmony/6.1-Release
```

脚本按序执行：opc patch.sh（fuzz）→ 恢复 musl 原版 ini → ohos-repos 补丁集 →
trio 增量补丁 → untracked 新文件。补丁内容索引见 `patches/` 目录。

## 4. SDK 组装（hap 编译必需，见 scripts/setup-sdk.sh 头注释）

```bash
# 4.1 公开 SDK（host 侧 ets/js/previewer/toolchains 二进制）
wget https://repo.huaweicloud.com/openharmony/os/6.1-Release/ohos-sdk-windows_linux-public.tar.gz
# 4.2 源码 SDK（systemapi typings + native sysroot）
source <python3.10 venv>/bin/activate && ./build.sh --product-name ohos-sdk --ccache
# 4.3 混装
bash scripts/setup-sdk.sh /root/OpenHarmony/6.1-Release <下载的tar.gz>
```

注意：源码 ohos-sdk 产品构建还需 host 侧 glibc2.39 修复（见 §5 编译 flag 一条）。

## 5. 构建

```bash
apt-get install -y autoconf automake libtool nasm autoconf-archive   # libnl/libvpx 需要
source /root/oh_env61/bin/activate        # python3.10 venv
./build.sh --product-name x86_general --ccache
```

**Ubuntu 24.04 + glibc 2.39 必须**（已含在补丁集 11-build.patch）：host 编译 cflags_cc
加 `-include string`（skia/ace 不再传递包含 <string>）。

**system.img 配额**：6.1 system 内容 ~2.2G，默认 2G 配额会在 e2fsdroid 阶段报
`Could not allocate block`。补丁集已把 `build/ohos/images/mkimage/system_image_conf.txt`
提到 4G；重拼盘也用 4G 分区。

**内存/磁盘**：构建峰值 ~50G 磁盘（out 98G + ccache 31G，定期 `ccache -M 8G`），内存 8G+。

## 6. 拼盘与启动

```bash
cd <staging>   # bzImage/ramdisk/system/vendor/userdata.img + scripts/disk_assemble.sh
cp <tree>/out/x86_general/packages/phone/images/{bzImage,ramdisk.img,system.img,vendor.img,userdata.img} .
bash scripts/disk_assemble.sh ./ohos_disk_61.img
bash scripts/fixup-disk.sh ./ohos_disk_61.img <tree>/out/x86_general   # 必须！见 §8.3/8.4
bash ../common/launch_qemu.sh ./ohos_disk_61.img     # grub: root=PARTLABEL=system
```

## 7. 图形栈（当前为「借 5.1 成品」快速通道）

6.1 镜像自带 mesa（三仓 gpu/lib64 预置）是旧 emutls 版 → SystemUI/appspawn 崩溃循环
（内核级 dump 100+ 次/boot）。**已验证修复**：用 5.1 修复版 10 库（`tools/mesa-libs/`
即 `-fno-emulated-tls` 重建产物，构建方法见 ohos-5.1/patches/05 + apply-emutls-flag.sh）
替换镜像内对应文件（6.1 布局为平铺）：

| 5.1 产物 | 6.1 镜像目标 |
|----------|--------------|
| system/lib{EGL,GLESv1_CM,GLESv2,glapi,gbm}…so | /system/lib64/ 同名 |
| dri/libgallium_dri.so | /system/lib64/libgallium_dri.so |
| vendor/lib64/chipsetsdk/lib{EGL,GLESv1/2/3}_impl.so | /system/lib64/ 同名 |

树侧同步替换 `device/soc/opc/x86_general/hardware/gpu/lib64{,/chipsetsdk}`。
效果：appspawn 内核崩溃 dump **118 → 0**，boot 推进到 foundation 等后期服务。

## 8. 已解决（2026-10-27/28 大扫除，按发现序）

1. ~~**内核 slub BUG**~~：根因不是 virtio-gpu，而是上游 accesstokenid 驱动删根节点时把
   替换指针写进**按值传入的局部参数地址**，`g_token_perm_root` 悬垂 → 双重释放 →
   `BUG at mm/slub.c:448`。修复 = `ohos-5.1/patches/06`（传 `**root_addr` 写回真根）。
   证据：UFZ 下旧内核 51.6s 起 376 行转储 / 修复内核 0 行；生产 cmdline 92 分钟 soak 0 BUG。
2. ~~**foundation 崩溃循环（15s 一崩）**~~：`libapp_domain_verify` 静态初始化 →
   `RestoreconRecurse` → 无 file_contexts（build_selinux=false）时 `selabel_open`
   仅带 digest 选项调用，其失败清理路径 free 未初始化指针 → SEGV。修复 =
   `selinux_adapter` 的 selinux_restorecon.c 无路径选项时跳过 open
   （patches/25）。**效果：SystemUI/lockscreen 首次能就绪**。
3. ~~**multimodalinput 冻结循环 + composer_host 不启动**~~：根因 **6.1 运行时根本没有
   挂载 vendor/data 分区** —— init 在挂任何分区前读 `/vendor/etc/fstab.x86_general`
   （5.1 模型：vendor 树内嵌在 system 镜像根）。6.1 构建把 chipset 拆去独立 vendor.img
   而 fstab 不可达 → `/vendor` 空目录 → 全部 HDF host（composer/allocator/audio/
   input…）`pathArgs invalid` → mmi 在 `IDisplayComposerInterface::Get` 上冻结。
   修复 = **vendor 树合并进 p2 根 /vendor**（fixup-disk.sh [1/4]）。
4. ~~**根文件系统被降级只读 / 无 tmpfs**~~：fstab 可读后其对 sda4 按 ext4 跑
   resize2fs/e2fsck，而 sda4 是 f2fs（6.1 默认）→ 失败 → 降级。f2fs.ko 未构建
   （CONFIG_F2FS_FS=m 且无打包），直接**把 data 分区重格为 ext4**（fixup-disk.sh [2/4]）。
5. ~~**hdf_devhost 二进制缺失**~~：上游 13e1a393 把 hdf_devhost 从 hdf_core 拆到
   `drivers/peripheral/adapter`（part=drivers_adapter），产品 config 未含该组件 →
   修复 = vendor/opc config.json hdf 子系统加 `drivers_adapter`（opc-trio 补丁）。
6. ~~**hdc 握手失败**~~：是 2/3 的连带症状。foundation 存活 + composer 注册后
   SDK23 hdc 直连成功（tconn//file recv/shell/hidumper 全通）。
7. ~~**GL 路径**~~：**6.1 RS 无 5.1 式 CPU 回退，必须让 GL 真跑**（5.1 的
   egl_no_init marker 公式在 6.1 上适得其反——RS 直接不出帧）。已验证
   `eglInitialize OK ver 1.4`：graphic.cfg 给 render_service 加
   LIBGL_ALWAYS_SOFTWARE=1 + LIBGL_DRIVERS_PATH=/system/lib64，并补
   kms_swrast/swrast/virtio_gpu_dri 别名（fixup-disk.sh [3/4]）。wrapper 侧
   v4 stubs/诊断已并入 patches/21。

## 8b. 当前唯一卡点：RS 渲染循环未启动（0 帧）

一切就绪但 RS 主循环死寂：屏幕已建（RSScreen 1280x800@75、SetScreenPowerStatus On）、
composer 线程 EGL+swrast+ShaderCache 就绪、composer_host 已分配 client target
（dumb 1280x832 AR24）、vsync 发生器活着（~56Hz）、hdc/hidumper/fpsCount 全可用——
但 `FrameBufferSurface` BufferQueue 恒 [0x0]/0 buffers、`RefreshRateCounts` 空、
输入注入无任何 RS 响应。扫描输出为开机残留灰噪（0xAA 散点，偶发微变）。
下一步方向：RSMainThread 激活条件（RequestNextVSync 已发但主线程不跑）；
hdc 实验环境已就绪（`hidumper -s 10`、hilog、kill render_service 触发式取证法）；
另注意 `RegScreenRefresh`/`RegDisplayVBlankIdleCallback` 在 opc HAL 上 -2
（V1_3/1_4 入口缺失，RS 侧仅告警，但值得先在 HAL 侧补齐再回归）。

## 9. 6.1 与 5.1 的关键区别（勿混淆）

| 项 | 5.1 | 6.1 |
|----|-----|-----|
| 产品 config 组件键名 | `"name"` | `"component"` |
| opengl wrapper | wrapper+impl 分层（platformsdk/libEGL.so 包一层） | 同 wrapper 机制但 impl 平铺在 /system/lib64 |
| build_selinux | false 可启动 | **必须 false**（true 时 init param service 被 AVC 卡死 panic） |
| musl 加载器 ini | 5.1 定制版（acquiiescence=/vendor/bin:…） | **必须上游原版**（含 chipset-sdk-sp + /system/bin 场景；错版 → init 静默 _exit(127)） |
| 内核 | 自带 6.6.86 可用 | 自带 6.6.23 不可用，整树换 5.1 内核 |
| system.img | 2016M | 4096M |
| Node | 14.21.1 | 18.20.1（ets2panda 需要 node:diagnostics_channel） |
| mindspore/nnrt | 可构建 | 树内 nnacl 引用的文件不存在 → 从产品配置移除（AI 栈不可用） |
