# OpenHarmony 6.1 (API 23) x86_general 移植构建指南

> 状态（2026-10-27）：镜像可从源码完整构建并启动到用户态（init 全流程 + 服务拉起），
> 图形栈已借用 5.1 修复版 mesa（appspawn 崩溃归零），**内核 slub BUG 已根修**
> （accesstokenid 双重释放，见 ohos-5.1/patches/06 + 本 README §8.1）。**尚未完成**：
> 5.1 显示 HAL 补丁与 wrapper init-fail 移植、hdc(SDK23) 握手。详见文末「剩余工作」。

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

## 8. 剩余工作（按优先级）

1. ~~**内核 slub BUG**~~ **已解决**（2026-10-27）：根因不是 virtio-gpu，而是上游
   accesstokenid 驱动删根节点时把替换指针写进**按值传入的局部参数地址**，
   `g_token_perm_root` 悬垂 → 后续 add/remove 沿 Freed 遍历 → 双重释放 →
   `BUG at mm/slub.c:448`。修复 = `ohos-5.1/patches/06-kernel-accesstokenid-doublefree.patch`
   （传 `**root_addr` 写回真根）。证据链：`slub_debug=UFZ slab_nomerge` 下旧内核 51.6s 起
   376 行 `FIX access_token_node`/OS_IPC 转储；同参数修复内核 609s+ 0 行；
   无调试参数生产 cmdline 下旧内核 557s panic、修复内核 soak 通过。注意根因帧里的
   selinux/access_tokenid add|remove 帧曾误导排查方向——真实肇事者是 remove 路径。
2. **5.1 显示 HAL 补丁移植**（ohos-5.1/patches/03：ApplyPlane 几何 + 软 vsync + 镜像旁路）
   到 6.1 的 device/soc/opc display 源码 —— 结构有差异需手工适配。**尚未做**。
3. **wrapper init-fail 移植**（ohos-5.1/patches/02 的 egl_no_init 机制）到 6.1 graphic_2d。
4. hdc：SDK23 的 hdc 与 6.1 hdcd 握手失败（端口开、Connect failed），修好前用串口观察。

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
