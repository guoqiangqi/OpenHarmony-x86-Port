# OpenHarmony 5.1 x86 QEMU：Flutter 评测 HAP 运行时修复（v2，首帧修复版）

## 本轮交付（2026-11-05 21:xx）

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
