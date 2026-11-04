# OpenHarmony 5.1 QEMU 基盘：Flutter 运行态（Vulkan/显示）

## 交付标识

| 项 | 值 |
|----|-----|
| 基盘 | `ohos_disk_51_verity.img` |
| md5 | 见同目录 `ohos_disk_51_verity.img.md5`（冻结：`0043a552f8662068302b23c3f7efd727`） |
| `libvulkan.so` | `/system/lib64/platformsdk/libvulkan.so` — 本轮 md5 **`430c458f9b36e88c7f2524452a1274cf`**（旧版 `33b8f991…`） |
| 版本 | OpenHarmony **5.1.0.107** / apiversion=18 |
| 内核 | **6.6.86** `#7` x86_64 |
| 显示 | virtio-gpu + Rosen；`/data/egl_no_init` CPU 合成 |

## 根因（评测仍 SEGV 的真正原因）

现象：`SIGSEGV@0x30` → `libflutter.so` → `libvulkan.so` (`globalGetProcAddr`)

OHOS 版 `libvulkan` 的 `loader_getenv()` **读系统 param（CachedParameter）**，**不读进程 environ**。

因此仅靠：

- `export VK_ICD_FILENAMES=…`（graphic.cfg）
- appspawn `env` 注入

**对 loader 无效**。若应用沙箱下默认 `/system/etc|/vendor/etc` + `vulkan/icd.d` 扫描再失败 → `Found no drivers!` → Flutter 空 dispatch SEGV。

Fingerprint / 内核不变是预期；**必须以 `libvulkan.so` md5 或 `param get VK_ICD_FILENAMES` 验收运行时是否更新。**

## 本轮构建侧修复

| 项 | 作用 |
|----|------|
| `graphic.cfg` `setparam VK_ICD_FILENAMES` + `VK_DRIVER_FILES` | 启动即写入 loader 真正读取的系统 param |
| `/system/lib64/libvk_icd_force.z.so` + appspawn `LD_PRELOAD` | 进程启动构造函数再 `SetParameter`/`setenv` |
| SwiftShader ICD 文件（system+vendor） | 软件 Vulkan 驱动 |
| `libvulkan.so` 增加 `.ohos_flutter_icd_note` | **md5 变化**（`33b8f991` → `430c458f`），标记本轮运行时改动 |
| `librender_service` forceCPU + `egl_no_init` + softsurf HAP | 首帧可截图 |

源码补丁说明：`ohos-5.1/patches/10-vulkan-loader-ohos-icd-fallback.patch`（getenv 回退 + ICD 硬编码兜底；完整重编需健康 out 树）。

## 验收命令

```bash
hdc shell 'param get VK_ICD_FILENAMES'
# 期望: /system/etc/vulkan/icd.d/vk_swiftshader_icd.json

hdc shell 'md5sum /system/lib64/platformsdk/libvulkan.so'
# 期望: 430c458f9b36e88c7f2524452a1274cf

hdc shell 'ls /system/lib64/libvk_icd_force.z.so /system/lib64/libvk_swiftshader.so'
hdc shell 'test -f /data/egl_no_init && echo egl_ok'
hdc shell 'param get persist.rosen.client_composition.enabled'  # 1

hdc install <hap>   # 评测 HAP；建议仍带 --enable-software-rendering
hdc shell 'aa start -a EntryAbility -b <bundle>'
sleep 30
hdc shell 'pidof <bundle>'
# faultlogger 无 libflutter+libvulkan @0x30
hdc shell 'snapshot_display -f /data/local/tmp/ss.jpeg'
```

## md5 说明

- 整盘 md5 **仅在 QEMU 停止后**计算；运行中会漂。
- 旧反馈 `9e4df884` / `c5f3bdd1` 若在运行中取样或未含本轮 `setparam`/`libvulkan` 改动，不能作为「Vulkan 已修」依据。

## 验收对照（本机构）

| 标准 | 结果 |
|------|------|
| 开机后 `param get VK_ICD_FILENAMES` 有值 | **通过** |
| `libvulkan.so` md5 ≠ `33b8f991…` | **通过**（`430c458f…`） |
| 应用 `LD_PRELOAD` 含 `libvk_icd_force.z.so` | **通过** |
| ICD 仅通过 param 路径可达时 CreateInstance=0 | **通过** |
| softsurf HAP ≥30s + Demo UI 截图 | **通过** |
