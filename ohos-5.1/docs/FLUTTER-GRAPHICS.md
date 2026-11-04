# OpenHarmony 5.1 QEMU 基盘：Flutter 运行态（Vulkan/显示）

## 交付标识

| 项 | 值 |
|----|-----|
| 基盘 | `ohos_disk_51_verity.img` |
| md5 | 见同目录 `ohos_disk_51_verity.img.md5`（冻结：`c5f3bdd18837084dd67b55646d6d66fd`） |
| 版本 | OpenHarmony **5.1.0.107** / apiversion=18 / general pc / tablet |
| 内核 | **6.6.86** `#7 SMP PREEMPT_DYNAMIC Tue Nov 3 20:08:22 CST 2026` x86_64 |
| 显示 | QEMU `virtio-gpu-pci` + Rosen；`/data/egl_no_init` CPU 合成兜底 |

## 根因与修复（相对「一启动 SIGSEGV」）

**现象**：`hdc install` / `aa start` 成功后进程立刻退出；faultlogger：

`SIGSEGV(SEGV_MAPERR)@0x30` → `libflutter.so` → `libvulkan.so` (`vkGetInstanceProcAddr` / `globalGetProcAddr`)

**根因**：guest 有 Vulkan loader（`/system/lib64/platformsdk/libvulkan.so`），但 **没有可用 ICD**（`vkCreateInstance: Found no drivers!` → 空 dispatch → SEGV）。

**基盘修复（软件 Vulkan / 应用侧可见）**：

1. SwiftShader：`/system/lib64/libvk_swiftshader.so`（并复制 `/vendor/lib64/libvk_swiftshader.so`）
2. ICD JSON：
   - `/system/etc/vulkan/icd.d/vk_swiftshader_icd.json` → `library_path=/system/lib64/libvk_swiftshader.so`
   - `/vendor/etc/vulkan/icd.d/vk_swiftshader_icd.json` → `library_path=/vendor/lib64/libvk_swiftshader.so`（沙箱 `app-base` 已 bind `/vendor/etc/vulkan`）
3. `graphic.cfg`：`export VK_ICD_FILENAMES /system/etc/vulkan/icd.d/vk_swiftshader_icd.json` + `render_service` env
4. **`appspawn.cfg` service env**（加固应用进程继承）：
   - `VK_ICD_FILENAMES=/system/etc/vulkan/icd.d/vk_swiftshader_icd.json`
   - `LIBGL_DRIVERS_PATH=/system/lib64/dri`
   - `LIBGL_ALWAYS_SOFTWARE=1`

实测：应用进程 `/proc/<pid>/environ` 已含 `VK_ICD_FILENAMES`；在应用 mount ns 内 `vk_probe2` → `CreateInstance r=0`、`physdev count=2`。隐藏 ICD 时可复现 `Found no drivers!` / `CreateInstance r=-9`。

## 首帧可截图（本轮关键）

白屏根因链：

1. `/data/egl_no_init` 强制 `eglInitialize` 失败 → RS 走 CPU 合成（正常 UI 可见）
2. Flutter XComponent SURFACE 的 buffer 在 uni-render 里默认走 `DrawImage`（EGLImage/OES）→ 在本栈为空纹理
3. 结果：`FlutterViewOnFirstFrame` 已触发，但 `snapshot_display` 中间仍是白洞

**修复**：

| 补丁 | 作用 |
|------|------|
| `librender_service.z.so` | `DealWithSelfDrawingNodeBuffer` 调用 `CreateBufferDrawParam(..., forceCPU=true)`（两处 `xor edx,edx` → `mov $1,%dl`），强制 `DrawBuffer(CPU)` 把 producer buffer 画进 uni-render 帧缓冲 |
| `rosen.client_composition.enabled=1`（已 `persist.rosen.client_composition.enabled=1`） | 强制 client composition，避免仅 HWC 挖洞 |
| Flutter HAP：`--enable-software-rendering` | 引擎走 `OhosSurfaceSoftware`，把 Skia 帧 `RequestBuffer`/`FlushBuffer` 写入 NativeWindow（不依赖 app EGL） |

参考截图：`vulkan-soft/flutter_icd_softsurf2.jpeg`（Demo Home Page + DEBUG 角标 + FAB）。

## 额外图形相关补丁（累计）

| 补丁 | 作用 |
|------|------|
| `/data/egl_no_init` | 强制 RS 跳过 `eglInitialize`（virtio-gpu EGL 写 dumb buffer 会丢像素）；**去掉会导致整屏黑屏** |
| `LIBGL_ALWAYS_SOFTWARE=1` + `LIBGL_DRIVERS_PATH` | RS / Mesa softpipe\|swrast；appspawn 同步注入 |
| `ld-musl-namespace-x86_64.ini` + `dri/tls/*` | app 进程可加载 Mesa dri |
| `librender_service.z.so` forceCPU | Flutter/XComponent SURFACE 首帧可截图 |
| vendor ICD + `libvk_swiftshader.so` | 双路径兜底，避免仅 system 扫描失败 |

## 可复制启用步骤（评测 skill）

基盘已默认启用。验证：

```bash
hdc shell 'test -f /data/egl_no_init && echo egl_no_init_ok'
hdc shell 'param get persist.rosen.client_composition.enabled'   # 期望 1
hdc shell 'ls -l /system/lib64/libvk_swiftshader.so /vendor/lib64/libvk_swiftshader.so /system/etc/vulkan/icd.d/ /vendor/etc/vulkan/icd.d/'
hdc shell 'grep VK_ICD /system/etc/init/graphic.cfg /system/etc/init/appspawn.cfg'
# 建议使用带 --enable-software-rendering 的 HAP（如 hello_world-softsurf-signed.hap）
hdc install hello_world-softsurf-signed.hap
hdc shell 'aa start -a EntryAbility -b com.example.hello_world'
sleep 30
hdc shell 'pidof com.example.hello_world'
hdc shell 'snapshot_display -f /data/local/tmp/ss.jpeg'
hdc file recv /data/local/tmp/ss.jpeg ./ss.jpeg
# faultlogger 不应出现 libflutter+libvulkan 同栈 cppcrash
# hilog 不应再刷 vkCreateInstance: Found no drivers!
```

HAP 侧启用软件渲染（embedding `FlutterLoader.ensureInitializationComplete`）：

```text
shellArgs.push("--enable-software-rendering");
```

## md5 说明

- **仅在 QEMU 完全停止后**对 `ohos_disk_51_verity.img` 做 `md5sum`；运行中整盘持续写入，md5 必然变化。
- 旧文档里的 `37f50012…` / 运行中记录的 `9e4df884…` 都不是冻结交付值；以停机后写入的 `ohos_disk_51_verity.img.md5` 为准。
- scp 传输出错不会改 md5；「本机与评测侧不一致」优先查是否对 **仍在运行的盘** 算了哈希。

## 验收对照（本机构实测）

| 标准 | 结果 |
|------|------|
| hdc install 成功 | **通过** |
| aa start 成功 | **通过** |
| 进程存活 ≥30s | **通过** |
| faultlogger 无 libflutter+libvulkan 同栈 cppcrash | **通过**（ICD 在位） |
| 应用 environ 含 `VK_ICD_FILENAMES` | **通过** |
| 应用 mount ns 内 Vulkan CreateInstance | **通过** |
| hdc screenshot 见 Flutter UI（计数器 Demo） | **通过**（softsurf HAP） |

## 残留说明

- `RegisterBuffer ... is not supported` / `SetMetadata -5` 仍可能刷屏，对首帧 CPU blit **非阻断**
- 去掉 `egl_no_init` 仍会导致黑屏（GL→dumb 丢像素）；勿在评测中移除
- Impeller / 真 Vulkan WSI 未作为本盘默认路径；无 `--enable-software-rendering` 的 HAP 走 `OhosSurfaceGLSkia`，在本栈可能白屏但仍不应因空 ICD SEGV
- `/data` 空间紧张时大 HAP 会 `9568288`（磁盘不足）；先 `bm uninstall` / 清 `/data/local/tmp/*.hap`

## 非目标（已满足）

- 不改签名 / 9568393 / fs-verity  
- 不要求改 Flutter 业务示例才能「不崩」（ICD 在基盘侧）  
