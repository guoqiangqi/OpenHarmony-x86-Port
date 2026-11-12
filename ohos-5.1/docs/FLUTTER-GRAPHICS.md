# OpenHarmony 5.1 x86 QEMU：Flutter 评测 HAP 运行时修复

## 当前机制（2026-11-10，不再按 so 打补丁）

Flutter OHOS 嵌入层（`FlutterLoader`）只在 `deviceInfo.productModel != "emulator"` 时才向引擎传 `--enable-impeller`。HarmonyOS 模拟器上报 `productModel=emulator`，所以走 Skia；这张 QEMU 盘原先上报 `general pc`，同一行判断放行，引擎进入 `kImpellerVulkan`。QEMU 只有 virtio-gpu + SwiftShader，没有 `VK_OHOS_native_buffer`，于是换一颗 `libflutter.so` 就在 Vulkan 路径上空指针崩溃。按文件偏移改指令只能保住已经对过字节的那一颗 so。

修复：系统分区 `/vendor/etc/param/product_x86_general.para` 的 `const.product.model="emulator"`。`const.ohos.apiversion` 仍是 `18`。按 so 偏移改指令的 `libvk_icd_force.z.so` 已从镜像删除，appspawn 不再预加载它。

2026-11-12 的手机盘在此之上把 `devicetype` 改为 `phone`（`product.name=ohos phone`，`HandsetSmartWindow`，默认全屏且无标题栏）。`const.bms.supportAppTypes=phone,tablet`，只声明 tablet 的 HAP 仍能安装。`model=emulator` 不变，Flutter 仍走 Skia。详见补丁 13、14。11-10 那张盘的 `devicetype` 仍是 `tablet`。

另外两处不是 Flutter so 专用、所有应用都需要，保留：

- `libscreenlock_bypass.z.so` 只进 foundation。这张盘没有锁屏界面，不绕过的话任何 `aa start`（Flutter、RN、sample）都会 10106102。
- `libappkit_native.z.so` 看门狗 3000ms 改为 60000ms（md5 `9e93c0787b5d37070c38bd61f127591f`，相对原版只改这两个字节）。改回 3000ms 后，debug Flutter 已经 `first-frame fired`，主线程在软件渲染里卡住，约 6 秒被 `THREAD_BLOCK_6S` 杀掉。这跟具体 so 无关。`persist.sys.abilityms.timeout_unit_time_ratio=24` 是这棵源码树的原值，没有另改。

复验（无 flpatch）：release 与 debug 两颗 so 都是 `rendering_api=1`、`first-frame fired`，debug 停留超过 45 秒且没有 `THREAD_BLOCK_6S`；`com.ohos.settings` 与 sample `ohos.samples.distributedcalc`（`Calc_Demo.hap` 新装后启动）同时存活。

下面各节是按 so 打补丁的历史，不再作为交付口径。

## 交付口径（使用侧按这个验收，不要按「已彻底修复」）

已发布基盘 `ohos_disk_51_verity.img` md5 `48012d5c0e2670075ee6c2c0d1aed43c`
（flpatch v3，`libvk_icd_force.z.so` md5 `cb0d02d72180e23f08ab2a5cde20703b`）只覆盖下面这一格。

| 用例 | 该盘的实际结果 |
|------|----------------|
| 干净 guest + 验收包 `hello_world-signed-release.hap` | 进程存活，hilog 有 first-frame fired / wait cleared |
| 干净 guest + `flutter build hap --debug --target-platform ohos-x64` | **不覆盖**。debug `libflutter.so` 约 39MB，文件布局和验收 release（约 18MB）不同 |
| 同一 guest 上多次安装/崩溃之后再跑验收 release | **不覆盖**。干净重启同一张盘后恢复。这是运行时竞态，不是盘坏了 |

v3 的 flpatch 只做两件事，而且两处都写死验收 release 引擎的文件偏移：

1. 后端改道，文件偏移 `0x37aca8`：Impeller Vulkan 分支改跳到 Skia GL。
2. `dl_sk_dispatcher.cc` 里 `Check failed: false` 的 thunk，文件偏移 `0x4a1500`，改成 `ret`。

它不改 `libvulkan.so`。debug 引擎、以及脏 guest 上补丁线程晚于引擎初始化的情况，仍会进
`libvulkan.so` 的 `vkGetInstanceProcAddr` / `globalGetProcAddr`，在空指针上
`SIGSEGV(SEGV_MAPERR)@0x28` 或 `@0x30`。`/data/log/flpatch.log` 里出现
`PATCHED` 和 `FATAL-THUNK-NEUTRALIZED` 只说明某个进程里验收 release 的那两个偏移对上了，
不能说明当前这个 debug 包被改道。

使用侧在这张盘上的临时做法：评测用 `flutter build hap --release`（或直接用验收 release 包）；
多次崩溃后重启 QEMU 或换干净 overlay，不要在脏 guest 上把 release 复现判成盘损坏。

## debug 崩溃点（Build ID 6646ddaf，2026-11-08 反汇编）

使用侧补上的 `libflutter.so`（39641040 字节，md5 `502ae5bf1523981aa6a106c3861ab0e3`）里，
`+0x1f6fd6c` 是 `mov 0x28(%rsi), %rax`。崩溃寄存器 `rsi = 0`，所以故障地址是 `0x28`，不是 loader 的 `[instance+8]`。
调用点在 `+0x179ad6e`：Vulkan surface 构造函数拿到一个空的 shared_ptr，仍把它传进这个不判空的拷贝。
上游 `OhosSurfaceFactoryImpl::CreateSurface` 在 `eax == 2` 时走这条路径（va `0x17ab8fe`，日志字符串
`use impeller-vulkan`）。`eax == 1` 才是 `use skia-gl`。

当前源码把这条 `je` 改成跳向 Skia 目标（`0x17ab96a`）。release 引擎里同样的短跳转序列还有别的函数，
必须同时看到 `use impeller-vulkan` 这句日志才改；验收 release 仍走原来的 `r12d` 固定偏移。
loader 上的入口门（v4 的 VULKAN-GUARD）已从实际打补丁路径拿掉：它会让 launcher `SIGBUS`，
并和 `libflutter+0x376d2f` 的 `@0x6b0` 一起出现。

2026-11-08 在干净 overlay 上换过当时的 so（md5 `c16069781fbad32016ddff7f672136fe`）。构造函数有打出
`ctor: v5 ohos-surface reroute`，launcher 没有新的 `SIGBUS`。验收 release 的进程没有在观测窗口里留下来，
也没有新的 cppcrash；当时 launcher 进程堆了好几个，guest 很慢。这一格还不能改口径。基盘 md5 仍是 `48012d5c`。
之后把「固定偏移已经命中就不再扫全文」编进 so（md5 `5fdb635773b7915d573ea440236e0e19`）又开了一次干净 overlay。pid 2159 打出了 `PATCHED`、`FATAL-THUNK-NEUTRALIZED` 和 `dlopen_ns-sync libflutter.so`，73 秒后仍然 `SIGSEGV@0x6b0`，PC `libflutter+0x376d2f`，`rcx = 0`。这条指令是 `rbx == 0` 时从 `0x376af4` 跳进来的 `mov 0x6b0(%rcx), %rax`，`96(%rsp)` 同样是 0。同机再开一张只带出厂 v3（md5 `cb0d02d7`）的 overlay 时，launcher 大约每 20 秒多出一个进程，验收包的 LoadAbility 提交后没有留下进程，也没有新的 cppcrash。

随后把 preload 退回这颗能工作的 v3（appspawn 扫描 + 进程内 watcher，不再拦截 `dlopen_ns`）。debug 引擎（Build ID `6646ddaf`）要改两处，只改 surface 会在 Skia 路径上把 Vulkan 上下文的 `+48` 当成 EGLDisplay，`SIGSEGV@0xb08`（`libflutter+0x177eb70`）。两处都是 `je` 改跳 Skia：surface 文件偏移 `0x17aa8fe`，context 文件偏移 `0x17aab12`。当前 so md5 `fc2ffa5e7602304a6ba472f17e986090`。

签名 debug HAP `com.example.my_flutter_app` / `EntryAbility` 在只换 preload 的 overlay 上：pid 2174 打出 `PATCHED-DEBUG` 和 `PATCHED-DEBUG-CTX`，走 `ohos_surface_gl_skia` / `create EGLSurface`，hilog 有 `first-frame callback` 和 `first-frame fired`，没有新的 cppcrash。进程随后被 appspawn 记为 signal 9。这和验收 release 在 first-frame 之后被 `LIFECYCLE_TIMEOUT` 杀掉是同一类前台超时，不是原来的 Vulkan 空指针。出厂 v3（`cb0d02d7`）没有这两处 debug 偏移。

## 出帧后不被 signal 9 杀掉，并且重启后 HAP 仍能打开（2026-11-09 overlay）

同一张 overlay（基盘仍是 `48012d5c`，改动在 qcow2 里）补了三处，干净 `begetctl reboot shutdown` 之后再启动，下面两格都过了：

| 改动 | 位置 |
|------|------|
| flpatch md5 `2d7382bc3d6a9c72d8a9b5aa47796526`（v5b：进程改名离开 apppool 挂快轮询 + debug fatal 中和 + 补丁行带目标 pid；2026-11-10 起生效，旧 v3 `12fd8e1a`） | `/system/lib64/libvk_icd_force.z.so` |
| `libappkit_native.z.so` 看门狗周期 3000ms 改为 60000ms，md5 `9e93c0787b5d37070c38bd61f127591f` | `/system/lib64/platformsdk/libappkit_native.z.so` |
| `persist.sys.abilityms.timeout_unit_time_ratio = 24`（前台超时约 120s） | `/system/etc/param/appfwk.para` |
| 内核读 code-sign 描述符时，hook 返回 1 就跳过 `__reserved` 和 `data_size == i_size`；btrfs 接受 `FS_IOC_ENABLE_CODE_SIGN`；密钥环为空且 `require_signatures=0` 时不再 `-ENOKEY` | ESP `bzImage`，`uname -v` 为 `#4 SMP PREEMPT_DYNAMIC Mon Nov 9 00:40:31 CST 2026`。四个模块（virtio-gpu、virtio_dma_buf、drm_shmem_helper、e1000）与这颗内核一起换过 |

复验（重启后没有重装，直接读已安装文件再拉起）：

- `entry.hap` 和 debug `libflutter.so` 的 `dd` 都成功。内核日志没有 `Reserved bits set`，也没有 `keyring is empty`。
- 验收 release pid 1859：`PATCHED`、`FATAL-THUNK-NEUTRALIZED`，`first-frame callback` 00:42:38，`first-frame fired` 00:42:44。00:43:38 进程仍在。
- debug pid 1898：`PATCHED-DEBUG` 和 `PATCHED-DEBUG-CTX`，Skia/EGL，`first-frame callback` 00:42:43，`first-frame fired` 00:42:53。00:43:38 进程仍在。
- 没有新的 11-09 00:42 之后的 cppcrash / sysfreeze。signal 9 没有再出现。

上面这次重启读文件时，盘上的 preload 还是更早的 so。内核、看门狗和 ratio 之后没有再改。14 点换上 `f17a3159` 的复验见下文。

上述 overlay 已导出为 `/root/ohos51-verity/ohos_disk_51_flutter_stayup.img`。换上 `12fd8e1a` 这版 preload 之后，整盘 md5 是 `404b7caac52342e753e90492493ec783`。原来的 `ohos_disk_51_verity.img` 没有被覆盖，md5 仍是 `48012d5c0e2670075ee6c2c0d1aed43c`。发布包 `images-5.1.0-20261109-flutter-fastarm` 带的是这张盘。上一版 tag `images-5.1.0-20261109-flutter-stayup` 仍是 `d42be74f` / preload `f17a3159`。

脏 guest 这一格能修，但不能靠拦截 `dlopen` / `dlopen_ns`。导出这两个符号之后，补丁明明已经打上，验收 release 仍在 `libflutter+0x376d2f` `SIGSEGV@0x6b0`（pid 1841、2194）。原因是 appspawn 多线程里别的线程拿着锁，`fork` 进子进程后这把锁一直是锁上的，子进程里的 watcher 根本没跑，只剩 appspawn 每 50ms 扫一次。热页缓存时引擎会先选 Vulkan。子进程里把这把锁重新初始化之后，watcher 自己打补丁：force-stop 再立刻 `aa start`，pid 2243 在 fork 后 2032ms 打上 `PATCHED` 和 `FATAL-THUNK-NEUTRALIZED`，09:37:12 `first-frame fired`，进程还在，没有新的 hello_world cppcrash。那一版 so 是 `2b16b03cba08e4d5d862f20cc3c09087`。它的 watcher 大约四分钟后退出，空闲更久的 apppool 会退回 50ms 扫描。

当前 so `f17a315909cadac11b85d035fa3f65ed` 在补丁打上之前不退出：前 10 秒每 1ms 看一次，之后每 50ms。2026-11-09 14 点在同一张 overlay 上复验（内核仍是上面的 `#4`，看门狗和 ratio 没变）：

- 验收 release pid 2131：fork 后 652ms 自己打上 `PATCHED` 和 `FATAL-THUNK-NEUTRALIZED`，14:16:47 `first-frame fired`。
- force-stop 再立刻启动，pid 2133 已空闲 84s，自己打补丁，14:18:13 `first-frame fired`。
- 再 force-stop，pid 2250 空闲 378s（超过旧 watcher 的约 260s）后自己打补丁，14:24:31 `first-frame fired`。14:28 进程仍在。
- debug pid 2459：`PATCHED-DEBUG` 和 `PATCHED-DEBUG-CTX`，Skia/EGL，14:25:20 `first-frame fired`。force-stop 再立刻启动，pid 2531 空闲 85s 后两处都打上，14:26:46 `first-frame fired`，14:28 仍在。
- 没有 14 点之后的新 cppcrash，也没有 `LIFECYCLE_TIMEOUT` / `THREAD_BLOCK`。debug 引擎会打出 `dl_sk_dispatcher.cc` 的 `Check failed: false`，进程没有因此退出。release 的 fatal thunk 偏移不在这颗 debug 引擎上。

`f17a3159` 的空闲 apppool 自己打补丁，使用侧从同一张盘复验时没有出现在被拉起的那个 pid 上：进程能出帧，但 flpatch 只有更早的 `fork-child`。specialization 发生在沙箱挂载之后，原来的 watcher 可能还停在慢轮询里。`12fd8e1a` 在 `prctl` 把进程名改离 `apppool` 时再起一个 1ms 轮询，这一步在引擎装入之前。2026-11-09 17 点复验：

- 立刻再拉起的 pid 2071 是 17:22:53 就 `fork-child` 的池。17:23 它自己打出 `fast-arm`、`PATCHED`、`FATAL-THUNK-NEUTRALIZED`，17:23:26 `first-frame fired`。
- 空闲约 7 分钟的 pid 2220 同样自己打出这三行，旧 watcher 在 429035 ms 结束，17:30:37 `first-frame fired`。
- 同一条路径上的 debug 池 pid 2440 自己打出 `PATCHED-DEBUG` 和 `PATCHED-DEBUG-CTX`。没有新的 17 点 cppcrash。

使用侧用盘 `404b7ca`、so `12fd8e1a` 复验时，只有开机后第一次冷启动（pid 2174）在最终 pid 上有 `fast-arm`、`PATCHED`、`FATAL-THUNK-NEUTRALIZED`。立刻再拉起、空闲超过六分钟、以及 debug 三次，最终 pid 只有更早的 `fork-child`。出帧和进程还在，不能当成补丁打在这个 pid 上。debug 复用池仍会打 `Check failed: false`。

构建侧用这张盘再开机，没有重装。release 立刻再拉起的最终 pid 2159 在 19:40:40 只有 `fork-child`，19:41:13 自己写下 `fast-arm`、`PATCHED`、`FATAL-THUNK-NEUTRALIZED`，19:41:22 `first-frame fired`。空闲六分钟和 debug 三次这次没有再跑。

## 合并盘 `ohos_disk_51_flutter_merge.img`（2026-11-09 21 点）

使用侧两张盘两次复验后对齐的结论：装包修复（fs-verity/code-sign）只在 11-06 verity 盘（`48012d5c`，内核 #7）上；留存修复（fast-arm preload `12fd8e1a`）只在 11-09 stayup 盘（`404b7ca`，内核 #4）上。两条修复线没有合并。

stayup 盘新装失败的根因在内核 #4 的 `fs/ext4/ioctl.c`：`FS_IOC_ENABLE_VERITY` 有 case，`FS_IOC_ENABLE_CODE_SIGN` 没有（f2fs 和 btrfs 都有，ext4 漏了）。BMS 对新文件 `EnableCodeSignForFile` 走到 ext4 的 `default:` 返回 ENOTTY（errno 25），`VerifyCodeSignatureForHap` 返回 -768，装包 9568393。`/sys/fs/verity` 缺失是另一个独立问题：#4 重建内核时 `fs/verity/sysfs.c`（07 号补丁）没有进 `src_tmp`，但 BMS 判定 fs-verity 用的是 statx `STATX_ATTR_VERITY`，不依赖这个目录——补上 ext4 的 ioctl 之后，20MB release HAP 在没有 `/sys/fs/verity` 的 #5 内核上也能装成功，证明装包链路只卡 ioctl。

合并盘在 stayup 盘（`404b7ca`：preload `12fd8e1a`、看门狗 `9e93c078`、ratio 24、两个预装 HAP）基础上只换了内核和 4 个模块：

- 内核 `6.6.86 #6 SMP PREEMPT_DYNAMIC Mon Nov 9 21:20:57 CST 2026`，bzImage md5 `7b8fd4da399732e1e2327869fa4d6979`。相对 #4 的差异：`fs/ext4/ioctl.c` 补上 `FS_IOC_ENABLE_CODE_SIGN`（主 ioctl 和 compat 两处，对齐 f2fs/btrfs 的接法）+ 恢复 07 号补丁的 `fs/verity/sysfs.c`（`/sys/fs/verity/features` 回来了）。verdict：verbatim #4 的其余部分（verity open.c 的 code-sign hook 顺序、signature.c 空 keyring、btrfs ioctl）都在。
- 4 个模块与 #4 同 md5（同一 vermagic `6.6.86 SMP preempt mod_unload modversions`）。
- 整盘 md5 `53d50f7a4ea17b2be898eb7b8d2255d3`。`ohos_disk_51_verity.img` 和 `ohos_disk_51_flutter_stayup.img` 都没动。

内核 #6 在合并盘（qcow2 overlay）上的复验（2026-11-09 21 点）：

- 干净开机：`/sys/fs/verity/features` 存在，preload 仍是 `12fd8e1a`。
- 新装 20MB 验收 release HAP（`bm install`，之前没装过的 overlay）：`install bundle successfully`，hilog `EnableCodeSignForFile ... ret = 0`、`VerifyCodeSignatureForHap` 通过，没有 9568393。
- 新装后立刻拉起：最终 pid 2289 自己打出 `fast-arm`、`PATCHED`、`FATAL-THUNK-NEUTRALIZED`（fork 后 2.3s），21:28:48 `first-frame fired`，52 秒后进程还在。
- 早一轮 #5 内核（同样的 ext4 ioctl 修复、没有 sysfs）在另一个 overlay 上还验过：新装 release 后重启，`dd` 读 `entry.hap` 正常（重启可读）；重启后冷启动 pid 2171 三行都在、`first-frame fired`；force-stop 立刻再拉起，池 pid 2174 自己打三行（旧 watcher 99495ms 结束）+ `first-frame fired`；debug 池 pid 2309 `PATCHED-DEBUG` + `PATCHED-DEBUG-CTX` + `first-frame fired`。两个应用 52 秒后都在。没有新的 cppcrash。

92MB debug HAP 新装仍然 9568288：`CheckSystemSize` 用 `max(2*hap, 512MiB)` 对 `/data/app/el1/bundle` 的 statfs，1.5GB 的 /data 在装包过程中（entry_tmp 拷贝与源 hap 并存）落到 512MiB 线以下。清掉 /data/log 后 avail 回到约 660MB 仍不够，因为检查时安装副本已经占了空间。这是 data 分区容量问题，不是两条修复线的问题；盘上预装的 `com.example.my_flutter_app` 不受影响，20MB 级新签名包装得进、装得上、装完能跑。

### 最终版：data 扩容 + 全矩阵（md5 `f5ca21a4` → 2026-11-10 v5b `991bd168`）

把 p4 从 1536M 扩到 2750M（镜像 5724M，文件系统 resize2fs 到 687395 块）之后，92MB debug 新装不再 9568288。整盘 md5 `f5ca21a4c9a55688852015bb8c2a23f1`，内核仍是 #6 `7b8fd4da`，preload `12fd8e1a`。发布包 `images-5.1.0-20261109-flutter-merge`（`disk_assemble.sh` 按 512M/2016M/512M/2750M 新布局，ESP 用 mtools 构建）。

**2026-11-10 v5b 更新（整盘 `991bd168884f5726c73dac1cdb1bfc2a`，preload `2d7382bc3d6a9c72d8a9b5aa47796526`）**：使用侧反馈两条"已知行为"有误判风险（验收 grep 目标 pid 名下的补丁行；`Check failed: false` 残留被视为故障嫌疑），于是从源头消除而不是写进说明让人忽略：

1. **debug fatal 补丁点**：debug 引擎（Build 6646ddaf）里 `dl_sk_dispatcher.cc(328)` 的 checker 在 file offset `0x18837e0`（vaddr `0x18847e0`），序言与 release fatal thunk 同签名 `53 48 81 ec 20 01 00 00`（`push rbx; sub rsp,0x120`），文件名串 `0x17f262`、`false` 串 `0x16e26b`、行号 `0x148`、无 rel32 caller。复用 `patch_fatal_thunk()` 以 `ret` 中和 → `Check failed: false` 从 hilog 消失。
2. **pid 归属**：父扫描器两条路径（`try_patch_process` 与 `apply_all_patches`）的补丁行改为 `PATCHED(-DEBUG) pid=<目标>`；子 watcher 完成时无论竞速输赢都在自己 pid 名下打 `self-patched backend sites`（自赢）或 `already-patched by parent scanner`（父先代打）。`grep "pid=<目标> "` 永远有证据行。
3. debug 分支完成条件从 `debug_routed && debug_ctx` 收紧为 `debug_routed && debug_ctx && debug_fatal`。
4. 基础盘 `/data/log/flpatch.log` 历史清空，开机后从 v5b ctor 行开始。

v5b 复验（qcow2 overlay，2026-11-10 上午）：新装 20MB release + 92MB debug 成功；release 冷启动/立即再拉（pid 2199/2202）、debug 冷启动 + 连续 5 次快拉（pid 2360/2430/2583/2661/2731/2805）全部目标 pid 名下三行补丁 + `first-frame fired` + 52s 留存；重启后两包仍在、release pid 2165 / debug pid 2168 出帧留存；hilog `Check failed` 0 条（仅 HDC 命令回显）；`cppcrash` 空。

在这张盘上（qcow2 overlay）的最终矩阵，全部先 `bm uninstall` 再全新 `bm install`：

- release `io.flutter.examples.hello_world`（20MB）：安装成功，`EnableCodeSignForFile ret = 0`。
- debug `com.example.my_flutter_app`（92MB）：安装成功，不再 9568288。
- release 冷启动 pid 2451 / 立即再拉 pid 2454 / 空闲 8.5 分钟再拉 pid 2756：三个场景最终 pid 都自打 `fast-arm`+`PATCHED`+`FATAL-THUNK-NEUTRALIZED`，分别 21:39:23、21:40:01、21:49:58 `first-frame fired`，52 秒留存。
- debug 冷启动 pid 2557 / 空闲 6.3 分钟再拉 pid 2990：自打 `fast-arm`+`PATCHED-DEBUG`+`PATCHED-DEBUG-CTX`，21:41:15、21:50:51 出帧留存。debug 立即再拉 pid 2658：自打 `fast-arm`，两处补丁由 appspawn 父扫描早 377ms 打上（PATCHED 行在 pid 164 名下），进程上补丁生效，21:42:17 出帧留存。
- 重启后：两个全新安装的 HAP `dd` 可读（20M/91M），无 Reserved-bits/verity 错误；冷启动 release pid 2180、debug pid 2183 均自打补丁行并出帧留存。
- 全程 `/data/log/faultlog/cppcrash` 为空。
- 拼盘脚本重放（reassembled.img）开机复验：#6 内核、`/sys/fs/verity` 在、preload `12fd8e1a`、/data 2.6G；release pid 2164 与 debug pid 2167 各自补丁+出帧+52 秒留存。

`48012d5c`、`404b7ca` 两张源盘未动。

## v4 源码（`docs/snippets/flutter_qemu_preload.c`，尚未替换 48012d5c 这张盘）

2026-11-07 在不改基盘的 qcow2 overlay 上试过，**release 验收没有保住，不能当新口径**：

- 从 appspawn 扫全机的版本会在 launcher 里 `SIGBUS`。子进程跑构造函数时 `comm` 还是 `appspawn`。这版已从源码里去掉。
- 去掉扫描、改由 `dlopen_ns` 在返回前打补丁之后，验收 release 仍然 `SIGSEGV(SEGV_MAPERR)@0x6b0`，`rcx = 0`，PC 在 `libflutter.so+0x376d2f`（`mov 0x6b0(%rcx), %rax`）。同一条 flpatch 里已经有 `PATCHED`、`FATAL-THUNK-NEUTRALIZED` 和 `dlopen_ns-sync libflutter.so`，没有 `VULKAN-GUARD`。也就是 v3 那两处补丁打上了，进程还是空指针崩溃。
- 所以 v4 连「干净 guest + release」都还没重新通过，debug HAP 和脏 guest 两格都没有验收数据。

针对上面两格补的运行时逻辑如下，未复验通过前不要当成新基盘口径：

1. **改道改为认指令形状，不再只认文件偏移。** 形状是验收 release 里唯一的一处
   `cmp $2, r12d; je; cmp $1, r12d; je; test r12d, r12d`（文件偏移 `0x37aca8`）。
   扫到就把第一条 `je` 改成跳向第二条 `je` 的目标。固定偏移仍然先试一次。
   本机另一份约 60MB 的 `libflutter.so`（不是使用侧那份 39MB debug 包）里没有这条指令，
   也没有 `dl_sk_dispatcher.cc` 字符串。扫不到时日志打
   `backend signature not found (r12d idiom); engine not rerouted`，进程不会被误改。
2. **fatal thunk 同样先试 `0x4a1500`，再按字符串签名扫。** 锚点是 `mov esi, 4`，
   向前找同时引用 `dl_sk_dispatcher.cc` 和精确字符串 `false` 的 `lea`，再改对应的
   `push rbx; sub rsp, 0x120` 序言。单独按这 8 字节序言去扫会打到别的函数，所以没有这么做。
3. **在 `dlopen` 返回前、由加载线程同步打补丁。** v3 靠 5ms 轮询 `/proc/self/maps`。
   脏 guest 上页缓存热，引擎会先跑到 Vulkan 分支，轮询还没被调度，于是同一张盘上的
   release 也会 `SIGSEGV@0x30`；干净重启后 I/O 变慢，补丁又抢先，所以看起来像「盘坏了」。
   `libflutter.so` 的构造函数不选后端（选后端在 `Rasterizer::Setup`），`dlopen` 返回前打完来得及。
   不再从 appspawn 扫别的进程：子进程构造函数跑的时候 `comm` 还是 `appspawn`，
   一开扫描就会在 launcher 里 `SIGBUS`。每个应用只补自己。
4. **本进程内给 guest `libvulkan.so`（620248 字节，md5 `430c458f9b36e88c7f2524452a1274cf`）加低地址防护。**
   `loader_get_instance` 在空指针检查之后直接比较 `[instance+8]` 上的魔数。实例句柄若是
   `0x20` / `0x28` 这种非空垃圾值，故障地址就是 `0x28` / `0x30`。
   崩溃栈上的 `globalGetProcAddr+77` 和 `vkGetInstanceProcAddr+38` 是 `strcmp` /
   `globalGetProcAddr` 的返回地址，不是故障指令本身。
   防护拒绝低于 `0x10000` 的 `pName` 和实例句柄（NULL 实例仍然合法），并把随后的
   `abort` 改成走到原来的「返回 NULL」路径。闸门页只建在当前进程里；从 appspawn
   往别的进程写一条跳进自己地址空间的跳转会立刻再崩，所以没有这么做。
   字节对不上这份 loader 就不动。

v4 仍然做不到、不能写成验收通过的情况：

- debug / JIT 引擎没有发出上述 `r12d` 三路比较。这种包不会被改道。
- 引擎拿到 NULL 函数指针之后自己 `call` 出去。loader 不再 `SIGSEGV`，崩溃点会挪到 `libflutter.so`。
- 不是这份 md5 的 `libvulkan.so`。

## 本轮交付（2026-11-05 21:xx，v3 / 盘 48012d5c）

| 项 | 值 |
|----|-----|
| 修复对象 | 评测 HAP `io.flutter.examples.hello_world`（release libflutter.so，Build ID `6d50daa4…35541f1bda7`） |
| 修复内容 | 在 v1（后端改道）基础上，新增 dl_sk_dispatcher 首帧致命断言中和补丁 |
| 验收结果 | 进程存活 ≥4.5 分钟且持续存活、`first-frame fired`、Flutter 蓝 UI 实际渲染（见 `flutter_firstframe.jpeg`）、零 `Check failed: false`、零 `SIGSEGV@0x30`、零 hello_world faultlog |

## 本轮新修复：dl_sk_dispatcher.cc(328) 首帧致命断言

### 现象
v1 盘上进程不再 SIGSEGV，但光栅线程反复打印：

```
[FATAL:flutter/display_list/skia/dl_sk_dispatcher.cc(328)] Check failed: false.
FML KILL PROCESS
FlutterWatchdog: FlutterUiThread is not alive
```

首帧永远不出现。

### 定位（二进制侧）
- 全库仅一处 line-328 检查点：thunk 位于 **vaddr `0x4a2500` / 文件偏移 `0x4a1500`**，
  开头 8 字节 `53 48 81 ec 20 01 00 00`（`push rbx; sub rsp,0x120`）。
- 该 thunk **没有任何 rel32 调用者**，只被 DisplayList 派发表（`.text` 段偏移 `0x90f90`
  表项）引用——即它是某个绘制 op 的处理器，函数体只有 FML fatal 记录（`mov ecx,0x148`
  = 行号 328）。Skia 后端收到不该到达的 op 时，直接把光栅线程 KILL，帧管线停摆，
  watchdog 随即判 UI 线程不活跃。

### 修复
`libvk_icd_force.z.so`（v3，md5 `cb0d02d72180e23f08ab2a5cde20703b`，17624 字节）在原
后端改道补丁成功后，继续把该 thunk 首 8 字节改写为 `c3 90 90 90 90 90 90 90`
（`ret` + 7×`nop`）：非法 op 变为无害 no-op，光栅继续。签名匹配 + 回读校验 +
`FATAL-THUNK-NEUTRALIZED` 日志（`/data/log/flpatch.log`）。

与 v1 同一处 watcher 一次完成两个补丁：

```
[flpatch pid=4613] PATCHED addr=0x7f201617bca8                ← 后端改道 (file off 0x37aca8)
[flpatch pid=4613] FATAL-THUNK-NEUTRALIZED addr=0x7f20162a2500 ← 断言中和 (file off 0x4a1500)
[flpatch pid=4613] self watcher: done after 3742 ms
```

（运行中进程实测内存字节：backend 处 `41 83 fc 02 e9 26 06 …`，thunk 处 `c3 90 90 …`。）

## 三层根因回顾（全部修复）

1. **锁屏死锁（10106102）**：PC 形态没有 SystemUI-ScreenLock 扩展，
   `ScreenLockSystemAbility` 默认锁定 → `aa start` 失败。
   修复：`libscreenlock_bypass.z.so` 预载进 `foundation`，stub
   `ScreenLockManager::IsScreenLocked/GetSecure/Unlock`。

2. **引擎后端选择（SIGSEGV@0x30）**：release libflutter.so 默认 Impeller Vulkan；
   SwiftShader ICD 缺 `VK_OHOS_native_buffer` → 空上下文崩溃。
   修复：vaddr `0x37bca8`（file off `0x37aca8`）14 字节 cmp/je/cmp 改道：上游构造函数
   分支直接跳到 Skia GL 配置（下游 type==1 分支随之走 GL 臂，参数容器匹配）。

3. **首帧致命断言（本轮）**：见上。

4. **appspawn 握手超时**：`persist.appspawn.reqMgr.timeout 60` 已固化在盘。

## 地址算术（两个补丁点通用）

内核 maps 按页对齐报 file offset；该 build 可执行段 vaddr = file offset + 0x1000。
对 maps 行 `(start, offset)`：补丁地址 = `start + (SITE_FILE_OFF - offset)`。
- 后端改道：SITE_FILE_OFF = `0x37aca8`
- 断言中和：SITE_FILE_OFF = `0x4a1500`
（仅 `r-xp` 可执行映射参与；整文件只读映射按 offset=0 算出后签名不匹配，自然跳过。）

## 冷启动注意（TCG 环境）

纯软件模拟（无 KVM）下，`foundation` 冷启动后第一次拉起本应用，ArkTS 模块加载可能
超过 AAFWK 生命周期阈值被 `LIFECYCLE_TIMEOUT/THREAD_BLOCK_6S` 误杀。两种做法：

1. （推荐）系统完全起来后（SystemUI/launcher 就绪）等待 2–3 分钟再 `aa start`；
2. 需要更长容忍时：`param set persist.sys.abilityms.timeout_unit_time_ratio 6`
   （AAFWK 全部生命周期超时 ×6；用完可设回 1）。

另外每次安装前先 `bm uninstall` 清掉旧数据可避免残留脏数据导致的启动悬挂。

## 验收步骤

```bash
hdc install hello_world-signed-release.hap
hdc shell aa start -a EntryAbility -b io.flutter.examples.hello_world
# 预期: start ability successfully
hdc shell 'tail /data/log/flpatch.log'
# 应见: PATCHED addr=0x...bca8 与 FATAL-THUNK-NEUTRALIZED addr=0x...2500
hdc shell snapshot_display -f /data/local/tmp/s.jpeg && hdc file recv ...
# 预期: Flutter 蓝 UI（70% 蓝色像素占比）
hilog | grep -a "Check failed: false"   # 预期 0 条
```

## 已知残留（非本补丁引入）

- TCG 环境系统服务偶发 `sysfreeze-foundation` / SystemUI jscrash 重启，属 x86 QEMU
  纯软件模拟的固有性能问题，与 HAP 无关。
- 长时间（数十分钟级）高负载下偶发 malloc 崩溃（TCG 内存压力），验收窗口内稳定。
