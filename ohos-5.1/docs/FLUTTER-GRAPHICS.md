# OpenHarmony 5.1 QEMU 基盘：Flutter 运行态（Vulkan/显示）

## 交付标识

| 项 | 值 |
|----|-----|
| 基盘 | `ohos_disk_51_verity.img` |
| 整盘 md5 | **`d9ac2bca57341b927e2c1db955fc746a`**（停 QEMU 后） |
| `libvulkan.so` | `/system/lib64/platformsdk/libvulkan.so` = **SwiftShader** md5 **`45e357165c667244523d5f5f14eec847`** |
| 旧 loader 备份 | `/system/lib64/platformsdk/libvulkan.loader_ohos.so`（原 OHOS loader） |
| 内核 | 6.6.86 `#7`；Fingerprint 可仍为 `8e452472…`（预期，未改 product 属性） |

## 为何 0043a552 / setparam 仍 SEGV

1. OHOS `libvulkan` 的 `loader_getenv` 读 **系统 param**，不是 environ。  
2. 上一轮只给 loader 加了 note（md5 `430c458f`），**loader 逻辑未重编进二进制**；`setparam` 在部分应用 DAC/时序下仍不足以保证 ICD。  
3. 评测 HAP 走 Vulkan 时仍可能 `Found no drivers!` → `SIGSEGV@0x30`（`globalGetProcAddr` / 空 dispatch）。  
4. 崩溃签名不变 ≠ 盘没更新；应用仍链到「会空 ICD」的 loader 行为。

## 本轮构建侧修复（实质性）

**将 `libvulkan.so` 直接替换为 SwiftShader 实现**（与 `libvk_swiftshader.so` 同内容）：

- 不再依赖 ICD 扫描 / `VK_ICD_FILENAMES` param / environ  
- `vkGetInstanceProcAddr` / `vkCreateInstance` 由 SwiftShader 直接提供  
- 空 ICD → SEGV 路径被拆除  

保留：`/data/egl_no_init`、RS forceCPU、`setparam`/ICD 文件、softsurf 建议。

## 验收

```bash
md5sum /system/lib64/platformsdk/libvulkan.so
# 期望: 45e357165c667244523d5f5f14eec847
# 且与 libvk_swiftshader.so 相同

ls -l /system/lib64/platformsdk/libvulkan.loader_ohos.so  # 旧 loader 备份存在即可

hdc install <hap>
aa start ...
sleep 30
# 无 libflutter→libvulkan @0x30 cppcrash
snapshot_display ...
```

## 说明

- Fingerprint / 内核号不变是正常的；请用 **`libvulkan.so` md5 = 45e35716…** 判断本轮是否生效。  
- 整盘请用 **`d9ac2bca…`**，不要再用 `0043a552` / `c5f3bdd1`。  
