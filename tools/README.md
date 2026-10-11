# MaliCompat CI Tools & Automation

## build_arm64_ci.sh

Automated GitHub Actions CI compilation dispatcher and artifact retriever for ARM64 C/C++ targets.

### Rule & Integrity Mandate
All ARM64 C/C++ binaries must be compiled via GitHub Actions CI (`CodeAbhi826/Malicomp`), never compiled directly on the host machine.

### Prerequisites
- GitHub CLI (`gh`) installed and authenticated (`gh auth status`).
- Git remote set to `https://github.com/CodeAbhi826/Malicomp.git`.

### Usage

```bash
# 1. Trigger remote CI build, wait for completion, and download artifacts to dist/:
./tools/build_arm64_ci.sh

# 2. Push staged commits and trigger CI build:
./tools/build_arm64_ci.sh --push -m "feat(ci): build and package all test binaries"

# 3. Retrieve artifacts from a known workflow run ID into dist/:
./tools/build_arm64_ci.sh --run-id 38124972423

# 4. Trigger build without waiting for completion (fire-and-forget):
./tools/build_arm64_ci.sh --no-watch
```

### Generated Artifacts in `dist/`
- `dist/libmalicompat.so`: ARM64 shared library (interceptor/HAL wrapper)
- `dist/malicompat-g71.zip`: Complete Adrenotools/custom driver package
- `dist/bin/malicompat-probe`: CLI Vulkan driver probe tool
- `dist/bin/malicompat-shim-test`: Descriptor shim conformance test
- `dist/bin/malicompat-bc-test`: BC texture decompression test
