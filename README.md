# MaliCompat

**MaliCompat** is a modular graphics compatibility layer designed to enable modern game renderers and native recompilation projects to run cleanly on ARM Mali GPUs when host Vulkan drivers lack features such as bindless descriptor indexing, hardware BC texture filtering, or stable occlusion query resets.

---

## Target Hardware & Testbed Environment
- **Device:** Samsung Galaxy A20 (`SM-A205F`)
- **SoC:** Samsung Exynos 7884B (ARM64 `armv8-a`, Cortex-A53 / Cortex-A73)
- **GPU:** ARM Mali-G71 MP2 (Bifrost 1st Gen, Vendor `0x13B5`, Core `0x60A00002`)
- **OS:** Feather Project 6.0 (Android 14 / One UI 6, API 34), Linux `4.4.302-p6`
- **Driver:** Samsung vendor Vulkan driver `v1.r26p0-01eac0` (Vulkan 1.1.131)

---

## Architectural Modules

### 1. `malicompat-core`
- **`CapabilityAnalyzer`:** Queries Vulkan physical devices, detects ARM Mali vendor quirks, evaluates descriptor limits, and selects the optimal compatibility route.
- **`DescriptorVirtualizer`:** Remaps SPIR-V descriptor sets and bindings to fit within hardware constraints (`maxBoundDescriptorSets = 4`).
- **`VulkanDescriptorShim`:** Intercepts `vkCreateDescriptorSetLayout`, sanitizes unsupported flags (`VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT`, `PARTIALLY_BOUND_BIT`), and manages shadow descriptor tables.
- **`BcTextureDecompressor`:** CPU-based decoder for BC1 (DXT1), BC3 (DXT5), BC4 (ATI1), and BC5 (ATI2) textures into RGBA8/R8/RG8 during Vulkan uploads.
- **`VulkanLayerExports`:** Exposes standard Vulkan layer and `LD_PRELOAD` symbols. Intercepts `vkGetPhysicalDeviceFeatures2` to inform engines that descriptor indexing features are available via virtualization and neuters occlusion queries (`VK_QUERY_TYPE_OCCLUSION`) to prevent Mali driver hangs.
- **`DriverArchivePacker`:** Generates standard `meta.json` manifests for packaging into custom driver ZIP archives compatible with Android loaders.

### 2. Standalone Verification Tools
- **`malicompat-probe`:** Standalone CLI tool to probe GPU capabilities and driver versions directly on Android over ADB.
- **`malicompat-shim-test`:** Conformance test validating that bindless descriptor layout creation succeeds on Mali-G71 without driver faults.
- **`malicompat-bc-test`:** Unit test verifying BC1–BC5 texture decoding accuracy on ARM64.

---

## Building
All builds are compiled in the cloud using GitHub Actions for Android ARM64 (`arm64-v8a`) targeting API 28+:
- **CI Workflow:** `.github/workflows/ci.yml`
- **Compiler:** Clang NDK r27b with `-march=armv8-a` baseline enforcement.

---

## Attributions & Compliance

All adapted techniques are attributed in `README.md`:
- `codepdbh/nfsmw-android` & `codepdbh/NFS-CARBON-360-ANDROID-RECOMP` by Paulo Daniel Batuani Hurtado (GPL-3.0)
- `victorgbd/NFSMW-Recompiled-Mobile` by victorgbd (GPL-3.0)
- `FristOneRR/FristOneRR-Panvk-Driver` by FristOneRR (GPL-3.0 / MIT)
- `leegao/bionic-vulkan-wrapper` by leegao (MIT)
- Mesa 3D Graphics Library / PanVK by Collabora, Arm Ltd., and contributors (MIT)


## Credits & Attributions
- [codepdbh/nfsmw-android](https://github.com/codepdbh/nfsmw-android) - Android port of Need for Speed Most Wanted (2005) recompilation.
- [WINDROID-EMU/NFSMW-RECOMP](https://github.com/WINDROID-EMU/NFSMW-RECOMP) & [victorgbd](https://github.com/victorgbd/NFSMW-Recompiled-Mobile) - 4-descriptor set Vulkan pipeline adaptation for mobile GPUs.
- [brunodev85/vortek](https://github.com/brunodev85/vortek) - Winlator Vortek Vulkan compatibility layer and Mali extension filtering techniques.
- [Mesa / PanVK](https://docs.mesa3d.org/drivers/panfrost.html) - Open source Arm Mali Vulkan driver research.
