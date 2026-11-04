# OpenHarmony 5.1 QEMU 基盘：Flutter 运行态（Vulkan/显示）

## 交付标识

| 项 | 值 |
|----|-----|
| 基盘 | `ohos_disk_51_verity.img` |
| md5 | 见同目录 `ohos_disk_51_verity.img.md5`（本轮：`37f50012d03389b4aabc5236e50c44b5`） |
| 版本 | OpenHarmony **5.1.0.107** / apiversion=18 / general pc / tablet |
| 内核 | **6.6.86** `#7 SMP PREEMPT_DYNAMIC Tue Nov 3 20:08:22 CST 2026` x86_64 |
| 显示 | QEMU `virtio-gpu-pci` + Rosen；`/data/egl_no_init` CPU 合成兜底 |

## 根因与修复（相对「一启动 SIGSEGV」）

**现象**：`hdc install` / `aa start` 成功后进程立刻退出；faultlogger：

`SIGSEGV(SEGV_MAPERR)@0x30` → `libflutter.so` → `libvulkan.so` (`vkGetInstanceProcAddr` / `globalGetProcAddr`)

**根因**：guest 有 Vulkan loader（`libvulkan.so`），但 **没有 ICD**。

**基盘修复（软件 Vulkan）**：

1. 注入 SwiftShader ICD：`/system/lib64/libvk_swiftshader.so`
2. ICD JSON：`/system/etc/vulkan/icd.d/vk_swiftshader_icd.json`
3. `graphic.cfg`：`VK_ICD_FILENAMES` + `render_service` env 同步

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

参考截图：`vulkan-soft/flutter_hello_firstframe.jpeg`（Demo Home Page + DEBUG 角标 + FAB）。

## 额外图形相关补丁（累计）

| 补丁 | 作用 |
|------|------|
| `/data/egl_no_init` | 强制 RS 跳过 `eglInitialize`（virtio-gpu EGL 写 dumb buffer 会丢像素）；**去掉会导致整屏黑屏** |
| `LIBGL_ALWAYS_SOFTWARE=1` + `LIBGL_DRIVERS_PATH` | RS / Mesa softpipe\|swrast |
| `ld-musl-namespace-x86_64.ini` + `dri/tls/*` | app 进程可加载 Mesa dri |
| `librender_service.z.so` forceCPU | Flutter/XComponent SURFACE 首帧可截图 |

## 可复制启用步骤（评测 skill）

基盘已默认启用。验证：

```bash
hdc shell 'test -f /data/egl_no_init && echo egl_no_init_ok'
hdc shell 'param get rosen.client_composition.enabled'   # 期望 1
hdc shell 'ls -l /system/lib64/libvk_swiftshader.so /system/etc/vulkan/icd.d/'
hdc install hello_world-signed.hap   # 含 x86_64 libflutter.so；建议带 --enable-software-rendering
hdc shell 'aa start -a EntryAbility -b com.example.hello_world'
sleep 30
hdc shell 'pidof com.example.hello_world'
hdc shell 'snapshot_display -f /data/local/tmp/ss.jpeg'
hdc file recv /data/local/tmp/ss.jpeg ./ss.jpeg
# faultlogger 不应出现 libflutter+libvulkan 同栈 cppcrash
```

HAP 侧启用软件渲染（embedding `FlutterLoader.ensureInitializationComplete`）：

```text
shellArgs.push("--enable-software-rendering");
```

## 验收对照（本机构实测）

| 标准 | 结果 |
|------|------|
| hdc install 成功 | **通过** |
| aa start 成功 | **通过** |
| 进程存活 ≥30s | **通过** |
| faultlogger 无 libflutter+libvulkan 同栈 cppcrash | **通过** |
| hdc screenshot 见 Flutter UI（计数器 Demo） | **通过** |

## 残留说明

- `RegisterBuffer ... is not supported` / `SetMetadata -5` 仍可能刷屏，对首帧 CPU blit **非阻断**
- 去掉 `egl_no_init` 仍会导致黑屏（GL→dumb 丢像素）；勿在评测中移除
- Impeller / 真 Vulkan WSI 未作为本盘默认路径

## 非目标（已满足）

- 不改签名 / 9568393 / fs-verity  
- 不要求改 Flutter 业务示例才能「不崩」（ICD 在基盘侧）  
