# OpenHarmony 5.1 x86 QEMU：Flutter 评测 HAP 运行时修复

## 本轮交付（2026-11-05，已交付级验证）

| 项 | 值 |
|----|-----|
| 交付 tarball | `OpenHarmony-5.1.0-x86_general-images-20261105-flutter.tar.gz`（1.6G），md5 `6432824b543e86e4b7964c9d3b331133` |
| 整盘（冻结） | `ohos_disk_51_verity.img` md5 `3559ee45f67c9710623f41aba227f679` |
| 修复对象 | 评测 HAP `io.flutter.examples.hello_world`（release libflutter.so，Build ID `6d50daa4…35541f1bda7`） |
| 验收 | 重建盘端到端：`aa start` 即成功、pid 2486 存活 35s+（冻结盘曾 25+ 分钟）、`PATCHED` 日志、Flutter 蓝 UI 渲染、无 `SIGSEGV@0x30`、无新增 cppcrash |

## 根因（三层，全部修复）

1. **锁屏死锁（10106102）**：PC 形态（`const.product.name = general pc`）没有独立锁屏扩展
   `SystemUI-ScreenLock.hap`，`ScreenLockSystemAbility::isScreenlockedMap_` 为空默认"锁定"，
   `aa start` 必现 `unlock screen failed in developer mode`。
   **修复**：`/system/lib64/libscreenlock_bypass.z.so`（源码 `snippets/screenlock_bypass.c`）
   LD_PRELOAD 进 `foundation`（`/system/etc/init/foundation.cfg` env），拦截
   `ScreenLockManager::IsScreenLocked/GetSecure/Unlock` 恒返回假/0。

2. **引擎渲染后端（SIGSEGV@0x30 / FML_CHECK platform_view_ohos.cpp:120）**：release libflutter.so
   默认 `"enable_impeller": "true"` 走 Impeller Vulkan；SwiftShader ICD 缺
   `VK_OHOS_native_buffer`，`ContextVK::Create` 返回 null → `ohos_context_` 空 → 崩。
   **修复**：`/system/lib64/libvk_icd_force.z.so`（源码 `snippets/flutter_qemu_preload.c`）
   LD_PRELOAD 进 `appspawn`。fork 应用子进程时经 `pthread_atfork` 装 watcher，引擎
   dlopen libflutter.so 后 5ms 轮询 `/proc/self/maps`，按 FILE_OFF 算术定位 vaddr `0x39cd12`
   的 6 字节分支 `0f 84 e9 00 00 00`（je → Impeller Vulkan），经 `/proc/self/mem` 改写为
   `e9 16 03 00 00 90`（jmp → 内建 Skia GL）。appspawn 根进程另跑 /proc 扫描器作备份。
   实测补丁 ~1.4s 内完成。

3. **appspawn 握手超时**：TCG 冷启 3s 不够，`persist.appspawn.reqMgr.timeout 60` 固化在盘。

## 地址算术（代码段候选地址）

内核 maps 按页对齐报 file offset。该 build 的可执行段把文件页 `0x2d2000` 映到 vaddr
`0x2d3000`（vaddr = file offset + 0x1000），目标指令 vaddr `0x39cd12` 位于文件偏移
`0x39bd12`。对 maps 行 `(start, end, offset)`：候选地址 = `start + (0x39bd12 - offset)`；
offset 为 0 的整文件映射（代码签名校验用）算出 `start + 0x39bd12`，该处 6 字节不匹配
签名自然跳过。

## 关键排障记录（本轮新发现）

- **LIFECYCLE_TIMEOUT sysfreeze**：应用主线程 `HandleLaunchApplication` 5.4s +
  `AbilityTransaction` 4s+ 超 AAFWK 预算 → 进程被 cgroup freezer 冻结（表现为"进程活着但
  引擎永不加载"）。触发链：nweb 预加载 fork nwebspawn → 早期版本 preload 的 clone hook 在
  nwebspawn 内 SIGBUS（strstr/candidates_of）→ 主进程等待卡死。
  **教训：preload 库绝不 hook `clone`，只 hook `fork()` + `pthread_atfork`**。
- host 无 KVM（TCG 纯软件模拟，8 vCPU ~313% host CPU）是所有生命周期超时的底层放大器。
- hdc shell 内 `grep` 处理二进制 cmdline 报 `Invalid argument`：先 `tr '\0' ' '` 再匹配，
  或直接用 `ps -ef | grep <name>`。

## 前一轮 libvulkan shim 方案的废弃说明

`1e80e177`（16832B）/`77a141f7`（15264B）两版 libvulkan shim 均未能阻止引擎初始化调度
函数内的崩溃——SwiftShader 没有 `VK_OHOS_native_buffer`，Impeller Vulkan 路径在该环境
**不可修补**。本轮起整条 Vulkan 路径改为"改道"（上文的 Skia GL 分支改写），盘上
`platformsdk/libvulkan.so` 已回退原厂（md5 `430c458f…`）。补丁 10（vulkan-loader
icd-fallback）保留作历史参考，不再是交付链路的一部分。

## 验收步骤

```bash
hdc install hello_world-signed-release.hap          # install bundle successfully
hdc shell 'aa start -a EntryAbility -b io.flutter.examples.hello_world'
# → start ability successfully（无 10106102）
sleep 30
hdc shell 'tail -3 /data/log/flpatch.log'           # PATCHED addr=0x...cd12/bca8
hdc shell 'ps -ef | grep hello_world'               # 进程在
hdc shell 'snapshot_display -f /data/local/tmp/s.jpeg'  # Flutter 蓝渐变 + 文本行
```

崩溃参照：补丁前 Fault thread 必落 `0x39cdxx-0x39dxxx`（Impeller Vulkan 分支）；补丁后
执行流走 `0x39d02d+`（Skia GL）。

## 已知残留（非本补丁引入）

- 无补丁旧实例存活 80 分钟后崩在 `__libc_malloc_impl`（TCG 长时高负载内存压力），与渲染路径无关。
- TCG 环境偶发 `sysfreeze-foundation` / SystemUI jscrash 重启，属 x86 QEMU 性能固有现象。
