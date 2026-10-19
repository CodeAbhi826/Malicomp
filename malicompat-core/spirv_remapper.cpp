#include "spirv_remapper.h"
#include "vulkan_layer_exports.h"
#include <atomic>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "MaliCompat-SPIRV"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { std::fprintf(stdout, "[MaliCompat-SPIRV] " __VA_ARGS__); std::fprintf(stdout, "\n"); } while(0)
#define LOGW(...) do { std::fprintf(stderr, "[MaliCompat-SPIRV-WARN] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { std::fprintf(stderr, "[MaliCompat-SPIRV-ERROR] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while(0)
#endif

namespace malicompat {

namespace {

// B9: shader-side descriptor probe for cause 53.
//
// A4/A6 clamp the SET LAYOUTS and POOLS to the driver's per-stage limits, yet the
// geometry pipelines still come back VK_ERROR_INITIALIZATION_FAILED (-3). The
// driver also validates what each VkShaderModule itself DECLARES at
// vkCreateGraphicsPipelines time: an interface array whose length is baked at
// 4096 (or a runtime array, or a non-uniform index -- all needing
// descriptorIndexing, which the M24 dump shows this G71 does not have) cannot be
// evaded by clamping the layout.
//
// So record, for every module that reaches this layer: its SPIR-V version and
// every UniformConstant variable with its set/binding decoration and the kind and
// array length of its type. The raw modules are also written to
// files/spirv_dump/sm_NNNN.spv for offline disassembly. Tracer only: the module
// handed to the driver is never modified.
uint32_t B9ProbeModule(const std::vector<uint32_t>& code) {
    static std::atomic<uint32_t> s_count{0};
    const uint32_t index = s_count.fetch_add(1);

    enum {
        kOpTypeImage = 25,
        kOpTypeSampler = 26,
        kOpTypeSampledImage = 27,
        kOpTypeArray = 28,
        kOpTypeRuntimeArray = 29,
        kOpTypePointer = 32,
        kOpConstant = 43,
    };
    std::unordered_map<uint32_t, uint32_t> constants;  // OpConstant id -> value
    std::unordered_map<uint32_t, uint32_t> typeOp;     // type id -> opcode
    std::unordered_map<uint32_t, uint32_t> arrayElem;  // array type id -> element type id
    std::unordered_map<uint32_t, uint32_t> arrayLen;   // array type id -> length (UINT32_MAX = runtime)
    std::unordered_map<uint32_t, uint32_t> pointerTo;  // pointer type id -> pointee id
    std::unordered_map<uint32_t, uint32_t> setOf;
    std::unordered_map<uint32_t, uint32_t> bindingOf;
    std::vector<std::pair<uint32_t, uint32_t>> vars;   // (variable id, result type id)

    for (size_t i = 5; i < code.size();) {
        const uint32_t len = code[i] >> 16, op = code[i] & 0xFFFF;
        if (len == 0 || i + len > code.size()) break;
        if (op == kOpConstant && len >= 4) constants[code[i + 2]] = code[i + 3];
        i += len;
    }
    for (size_t i = 5; i < code.size();) {
        const uint32_t len = code[i] >> 16, op = code[i] & 0xFFFF;
        if (len == 0 || i + len > code.size()) break;
        if (len >= 2 && (op == kOpTypeImage || op == kOpTypeSampler || op == kOpTypeSampledImage)) {
            typeOp[code[i + 1]] = op;
        } else if (op == kOpTypeArray && len >= 4) {
            typeOp[code[i + 1]] = op;
            arrayElem[code[i + 1]] = code[i + 2];
            const auto c = constants.find(code[i + 3]);
            arrayLen[code[i + 1]] = c != constants.end() ? c->second : 0xFFFFFFFEu;
        } else if (op == kOpTypeRuntimeArray && len >= 3) {
            typeOp[code[i + 1]] = op;
            arrayElem[code[i + 1]] = code[i + 2];
            arrayLen[code[i + 1]] = 0xFFFFFFFFu;
        } else if (op == kOpTypePointer && len >= 4) {
            pointerTo[code[i + 1]] = code[i + 3];
        } else if (op == SpirvRemapper::kOpDecorate && len >= 4) {
            if (code[i + 2] == SpirvRemapper::kDecorationDescriptorSet) setOf[code[i + 1]] = code[i + 3];
            else if (code[i + 2] == SpirvRemapper::kDecorationBinding) bindingOf[code[i + 1]] = code[i + 3];
        } else if (op == SpirvRemapper::kOpVariable && len >= 4 && code[i + 3] == 0 /*UniformConstant*/) {
            vars.emplace_back(code[i + 2], code[i + 1]);
        }
        i += len;
    }

    char summary[768];
    int off = 0;
    uint32_t shown = 0;
    off += snprintf(summary + off, sizeof(summary) - (size_t)off, "B9: sm#%u words=%zu version=%u.%u",
                    index, code.size(), (code[1] >> 16) & 0xFFu, (code[1] >> 8) & 0xFFu);
    for (const auto& v : vars) {
        if (shown == 12) {
            off += snprintf(summary + off, sizeof(summary) - (size_t)off, " ...(%zu more)",
                            vars.size() - shown);
            break;
        }
        ++shown;
        uint32_t t = v.second;
        const auto ptr = pointerTo.find(t);
        if (ptr != pointerTo.end() && typeOp.find(t) == typeOp.end()) t = ptr->second;
        uint32_t kindOp = 0;
        uint32_t len = 0;
        bool isArray = false;
        const auto tIt = typeOp.find(t);
        if (tIt != typeOp.end()) kindOp = tIt->second;
        if (kindOp == kOpTypeArray || kindOp == kOpTypeRuntimeArray) {
            isArray = true;
            len = arrayLen.count(t) ? arrayLen[t] : 0;
            const auto e = arrayElem.find(t);
            if (e != arrayElem.end()) {
                const auto inner = typeOp.find(e->second);
                kindOp = inner != typeOp.end() ? inner->second : 0;
            }
        }
        const char* kind = kindOp == kOpTypeImage       ? "img"
                           : kindOp == kOpTypeSampler   ? "smp"
                           : kindOp == kOpTypeSampledImage ? "simg"
                                                        : "?";
        char lenText[24];
        if (!isArray) snprintf(lenText, sizeof(lenText), "-");
        else if (len == 0xFFFFFFFFu) snprintf(lenText, sizeof(lenText), "rt");
        else if (len == 0xFFFFFFFEu) snprintf(lenText, sizeof(lenText), "?");
        else snprintf(lenText, sizeof(lenText), "%u", len);
        off += snprintf(summary + off, sizeof(summary) - (size_t)off, " [s%u b%u %s%s len=%s]",
                        setOf.count(v.first) ? setOf[v.first] : 0,
                        bindingOf.count(v.first) ? bindingOf[v.first] : 0,
                        kind, isArray ? "[]" : "", lenText);
    }
    LOGI("%s", summary);

    // Keep a copy of the exact module the driver compiles, bounded to 400 files.
    if (index < 400) {
        const char* dir = "/data/user/0/io.github.nfsmwrecomp/files/spirv_dump";
        mkdir(dir, 0755);
        char path[192];
        snprintf(path, sizeof(path), "%s/sm_%04u.spv", dir, index);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(code.data(), sizeof(uint32_t), code.size(), f);
            fclose(f);
        }
    }
    return index;
}

}  // namespace

SpirvRemapper& SpirvRemapper::Instance() {
    static SpirvRemapper instance;
    return instance;
}

bool SpirvRemapper::IsSpirv(const uint32_t* pCode, size_t wordCount) const {
    return pCode != nullptr && wordCount >= 5 && pCode[0] == kSpirvMagic;
}

uint32_t SpirvRemapper::GetSpirvVersion(const uint32_t* pCode, size_t wordCount) const {
    if (!IsSpirv(pCode, wordCount)) return 0;
    return pCode[1];
}

bool SpirvRemapper::DowngradeToSpirv13(const uint32_t* pCode, size_t wordCount, std::vector<uint32_t>& outCode) {
    if (!IsSpirv(pCode, wordCount)) return false;

    // Collect storage class for every OpVariable
    std::unordered_map<uint32_t, uint32_t> varClasses;
    for (size_t i = 5; i < wordCount;) {
        const uint32_t len = pCode[i] >> 16;
        const uint32_t op = pCode[i] & 0xFFFF;
        if (len == 0 || i + len > wordCount) return false;

        if (op == kOpVariable && len >= 4) {
            // OpVariable: [0]=len|op, [1]=res_type, [2]=res_id, [3]=storage_class
            varClasses[pCode[i + 2]] = pCode[i + 3];
        }
        i += len;
    }

    outCode.clear();
    outCode.reserve(wordCount);
    // Header (5 words)
    outCode.insert(outCode.end(), pCode, pCode + 5);
    outCode[1] = kSpirvVersion13; // Downgrade version word

    for (size_t i = 5; i < wordCount;) {
        const uint32_t len = pCode[i] >> 16;
        const uint32_t op = pCode[i] & 0xFFFF;

        if (op != kOpEntryPoint || len < 4) {
            outCode.insert(outCode.end(), pCode + i, pCode + i + len);
            i += len;
            continue;
        }

        // OpEntryPoint: execution_model, entry_id, name string (null-terminated), followed by interface ids
        size_t j = i + 3;
        for (; j < i + len; ++j) {
            const uint32_t w = pCode[j];
            if ((w & 0xFF) == 0 || ((w >> 8) & 0xFF) == 0 ||
                ((w >> 16) & 0xFF) == 0 || (w >> 24) == 0) {
                ++j;
                break;
            }
        }

        const size_t entryPointStart = outCode.size();
        outCode.insert(outCode.end(), pCode + i, pCode + j);

        // Filter interface operands to only Input and Output storage classes
        for (; j < i + len; ++j) {
            const uint32_t varId = pCode[j];
            const auto it = varClasses.find(varId);
            if (it != varClasses.end() && (it->second == kStorageClassInput || it->second == kStorageClassOutput)) {
                outCode.push_back(varId);
            }
        }

        // Update OpEntryPoint word count
        const uint32_t newLen = static_cast<uint32_t>(outCode.size() - entryPointStart);
        outCode[entryPointStart] = (newLen << 16) | kOpEntryPoint;
        i += len;
    }

    return true;
}

// M22: INTENTIONALLY NOT CALLED. The guest renderer already performs this exact
// descriptor-set packing itself (nfsmw_spirv_desificadores.h::JuntarConjuntos, from
// CrearModulo when maxBoundDescriptorSets < 5). Calling both collapsed distinct
// descriptor sets onto each other and broke every geometry pipeline. Kept only as
// documentation of the rule; do not re-enable without first proving the guest did
// not run, which is not something this layer can observe.
bool SpirvRemapper::RemapToFourSets(const uint32_t* pCode, size_t wordCount, std::vector<uint32_t>& outCode) {
    if (!IsSpirv(pCode, wordCount)) return false;

    // Pass 1: find and validate OpDecorate instructions for DescriptorSet & Binding
    std::unordered_map<uint32_t, uint32_t> conjuntos;
    const uint32_t bound = pCode[3];

    for (size_t i = 5; i < wordCount;) {
        const uint32_t len = pCode[i] >> 16;
        const uint32_t op = pCode[i] & 0xFFFF;
        if (len == 0 || i + len > wordCount) return false;

        if (op == kOpDecorate) {
            if (len < 3) return false;
            const uint32_t dec = pCode[i + 2];
            if (dec == kDecorationBinding || dec == kDecorationDescriptorSet) {
                if (len != 4 || pCode[i + 1] >= bound) return false;
                if (dec == kDecorationDescriptorSet) {
                    const uint32_t target = pCode[i + 1];
                    const uint32_t set = pCode[i + 3];
                    if (set > 4 || !conjuntos.emplace(target, set).second) return false;
                }
            }
        }
        i += len;
    }

    outCode.assign(pCode, pCode + wordCount);

    // Pass 2: mutate DescriptorSet and Binding values
    for (size_t i = 5; i < wordCount;) {
        const uint32_t len = outCode[i] >> 16;
        const uint32_t op = outCode[i] & 0xFFFF;
        if (op == kOpDecorate && len == 4) {
            const uint32_t target = outCode[i + 1];
            const auto it = conjuntos.find(target);
            if (it != conjuntos.end()) {
                const uint32_t origSet = it->second;
                const uint32_t dec = outCode[i + 2];
                if (dec == kDecorationDescriptorSet && origSet >= 2) {
                    outCode[i + 3] = origSet - 1;
                } else if (dec == kDecorationBinding && origSet == 2) {
                    outCode[i + 3] += 1;
                }
            }
        }
        i += len;
    }

    return true;
}

bool SpirvRemapper::RemapShader(const uint32_t* pCode, size_t wordCount, std::vector<uint32_t>& outCode) {
    if (!IsSpirv(pCode, wordCount)) return false;

    // M22: the guest renderer ALREADY does the four-descriptor-set packing itself.
    //
    // nfsmw's app/src/nfsmw_spirv_desitorios.h implements JuntarConjuntos, called
    // from CrearModulo when maxBoundDescriptorSets < 5 (Mali Valhall, which includes
    // our G71, reports 4). Its rule is character-for-character the same as the
    // RemapToFourSets() that used to live here:
    //
    //     OpDecorate DescriptorSet(34) with value >= 2   -> value - 1
    //     OpDecorate Binding(33) on a resource in set 2  -> value + 1
    //
    // Running both therefore remapped twice:
    //
    //     guest   2->1  3->2  4->3      => {0,1,2,3}
    //     layer   2->1  3->2            => {0,1,1,2}   <- sets 1 and 2 collide
    //
    // Distinct descriptor sets collapsed onto each other with overlapping bindings,
    // which is why every geometry pipeline came back VK_ERROR_INITIALIZATION_FAILED
    // and the guest reported dibujados=0 rechazados=13.
    //
    // Proof from the M20 run, same second, in order:
    //   22:15:03.777 [compatibilidad] 4 conjuntos de descriptores: el monton de
    //                      cubos va en el conjunto del 3D y samplers y UBO bajan uno
    //   22:17:18.634 MaliCompat-SPIRV: successfully remapped shader (3417 -> 3417)
    //
    // The guest owns the layout it built, so it is authoritative. Drop our copy.
    // What is left is the SPIR-V version clamp, which is idempotent: it only fires
    // above 1.3, and the guest has already logged "SPIR-V de la biblioteca
    // convertido a 1.3", so in practice this is a no-op safety net rather than a
    // second transformation.
    std::vector<uint32_t> tempCode;
    if (GetSpirvVersion(pCode, wordCount) > kSpirvVersion13) {
        LOGI("SpirvRemapper: shader is SPIR-V > 1.3; downgrading to 1.3");
        if (!DowngradeToSpirv13(pCode, wordCount, tempCode)) {
            LOGW("SpirvRemapper: Failed to downgrade SPIR-V version, proceeding with original");
            tempCode.assign(pCode, pCode + wordCount);
        }
    } else {
        tempCode.assign(pCode, pCode + wordCount);
    }

    // Pass the guest's own module through untouched. Still log the descriptor set
    // inventory so we can see, from the host side, exactly which sets the guest
    // produced instead of having to infer it from the game's own log. Shader
    // creation is rare, so this is not worth gating behind a debug level.
    {
        std::unordered_map<uint32_t, uint32_t> seen;
        const uint32_t bound = tempCode[3];
        for (size_t i = 5; i < tempCode.size();) {
            const uint32_t len = tempCode[i] >> 16;
            const uint32_t op = tempCode[i] & 0xFFFF;
            if (len == 0 || i + len > tempCode.size()) break;
            if (op == kOpDecorate && len == 4 && tempCode[i + 2] == kDecorationDescriptorSet &&
                tempCode[i + 1] < bound) {
                seen[tempCode[i + 1]] = tempCode[i + 3];
            }
            i += len;
        }
        char sets[64];
        int off = 0;
        for (const auto& kv : seen) {
            off += snprintf(sets + off, sizeof(sets) - off, "%s%u", off ? "," : "", kv.second);
            if (off >= (int)sizeof(sets) - 4) break;
        }
        LOGI("SpirvRemapper: passthrough (M22, no set remap); descriptor sets present: %s",
             off ? sets : "<none>");
    }

    outCode.swap(tempCode);
    return true;
}

VkResult SpirvRemapper::InterceptCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule,
    PFN_vkCreateShaderModule realCreateShaderModule) {

    if (!pCreateInfo || !pShaderModule) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const uint32_t* pCode = pCreateInfo->pCode;
    const size_t wordCount = pCreateInfo->codeSize / sizeof(uint32_t);

    if (IsSpirv(pCode, wordCount)) {
        std::vector<uint32_t> remappedCode;
        if (RemapShader(pCode, wordCount, remappedCode)) {
            LOGI("InterceptCreateShaderModule: successfully remapped shader (%zu -> %zu words)",
                 wordCount, remappedCode.size());
            // B9: this is the exact module every pipeline will compile. Record its
            // descriptor interface and keep a copy on disk before handing it over.
            const uint32_t b9_index = B9ProbeModule(remappedCode);
            VkShaderModuleCreateInfo modifiedInfo = *pCreateInfo;
            modifiedInfo.pCode = remappedCode.data();
            modifiedInfo.codeSize = remappedCode.size() * sizeof(uint32_t);
            if (realCreateShaderModule) {
                const VkResult b9_result =
                    realCreateShaderModule(device, &modifiedInfo, pAllocator, pShaderModule);
                LOGI("B9: sm#%u handle=%p result=%d", b9_index,
                     pShaderModule ? (void*)(*pShaderModule) : nullptr, (int)b9_result);
                return b9_result;
            }
            // M15: never hand out fake handles; the old 0x501234 mock crashed
            // the Mali driver inside vkCreateGraphicsPipelines (fault 0x401244).
            LOGE("InterceptCreateShaderModule: no vendor proc, failing loud");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    if (realCreateShaderModule) {
        return realCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
    }
    LOGE("InterceptCreateShaderModule: no vendor proc for passthrough, failing loud");
    return VK_ERROR_INITIALIZATION_FAILED;
}

} // namespace malicompat

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule) {
    // M15: resolve the vendor proc in the vendor dispatch domain (M6 helper)
    // instead of passing nullptr (which minted fake 0x501234 handles that
    // crashed Mali inside vkCreateGraphicsPipelines).
    PFN_vkCreateShaderModule realCreate = reinterpret_cast<PFN_vkCreateShaderModule>(
        malicompat_resolve_vendor_device_proc(device, "vkCreateShaderModule"));
    return malicompat::SpirvRemapper::Instance().InterceptCreateShaderModule(
        device, pCreateInfo, pAllocator, pShaderModule, realCreate);
}

} // extern "C"
