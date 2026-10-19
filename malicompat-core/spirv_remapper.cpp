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

// B10: rewrite descriptor arrays to lengths the Mali driver will accept.
//
// The B9 probe shows exactly what the driver refuses: the scene shaders declare
// their heaps as RUNTIME arrays of images and samplers
// ([s0 b0 img[] len=rt], [s2 b0 smp[] len=rt]). A runtime array in the shader
// interface requires runtimeDescriptorArray, which this G71 does not have (M24),
// so vkCreateGraphicsPipelines returns VK_ERROR_INITIALIZATION_FAILED (-3) for
// every geometry pipeline -- the 13-of-13 causa 53 the engine reports. Fixed
// arrays are not legal either while the guest keeps its own counts: 4096 sampled
// images and 512 samplers exceed maxPerStageDescriptorSampledImages (256) and
// maxPerStageDescriptorSamplers (128), and even the A4-clamped pair (256 + 128)
// overflows maxPerStageResources (361) inside a single stage.
//
// So give every descriptor array a fixed length taken from the guest's four-set
// shape (set 0 binding 0 -> 256 images, set 1 binding 0 -> 16, set 1 binding 1 ->
// 64, set 2 binding 0 -> 128 samplers), then shrink greedily so the per-type sums
// (256 images, 128 samplers) and the per-stage total all hold with a small
// margin. This is safe in practice: the guest allocates its heaps densely from
// slot 0 (A3 measured maxLogicalIndex 15 against ~26 live textures), the set
// layouts still hold their clamped capacity, and every slot is dummy-filled
// (F8), so even an out-of-range index lands on a valid dummy descriptor.
//
// The only thing this changes is array lengths and those two declarations: ids,
// decorations and every other instruction are left alone. Returns how many array
// types were rewritten (plus one when the declarations were also dropped).
//
// Run20 confirmed the arrays no longer decide the outcome: with them fixed the
// driver still returned -3 for the same 13 pipelines. The missing piece is the
// module's declaration list: the guest's translator also emits OpCapability
// RuntimeDescriptorArray and OpExtension "SPV_EXT_descriptor_indexing" (even in
// the vertex shaders that have no descriptors at all), because the M19 spoof made
// it believe the feature exists. vkCreateShaderModule accepts them (Mali only
// stores the module there), but the shader compiler runs at pipeline creation,
// where a declared capability the device lacks is a hard failure. With the
// runtime arrays gone the declarations are unused, so they are dropped here.
uint32_t B10ClampDescriptorArrays(std::vector<uint32_t>& code) {
    enum {
        kOpExtension = 10,
        kOpCapability = 17,
        kOpTypeInt = 21,
        kOpTypeImage = 25,
        kOpTypeSampler = 26,
        kOpTypeSampledImage = 27,
        kOpTypeArray = 28,
        kOpTypeRuntimeArray = 29,
        kOpTypePointer = 32,
        kOpConstant = 43,
        kOpFunction = 54,
        kCapabilityRuntimeDescriptorArray = 5302,
    };

    if (code.size() < 5 || code[0] != 0x07230203) return 0;

    // The A7 marker disables the A4/A6 descriptor clamps; honour it here too so
    // the null hypothesis stays testable.
    static const bool clampsDisabled = [] {
        FILE* f = fopen("/data/user/0/io.github.nfsmwrecomp/files/noclamp", "r");
        if (f) {
            fclose(f);
            return true;
        }
        return false;
    }();
    if (clampsDisabled) return 0;

    std::unordered_map<uint32_t, uint32_t> typeOp, arrayElem, arrayLenConst, pointerTo;
    std::unordered_map<uint32_t, uint32_t> setOf, bindingOf, constValue, constType;
    std::vector<std::pair<uint32_t, uint32_t>> vars;  // (variable id, result type id)
    std::vector<size_t> stripWords;  // word offsets of declaration instructions to drop
    uint32_t uintTypeId = 0;
    uint32_t uniformVarCount = 0;  // UBO variables, counted against the stage total

    // True when the OpExtension's literal string is exactly `wanted`.
    auto extensionIs = [&](size_t i, uint32_t len, const char* wanted) {
        const size_t wantedLen = strlen(wanted);
        if ((size_t)(len - 1) * 4 < wantedLen + 1) return false;
        const char* s = reinterpret_cast<const char*>(&code[i + 1]);
        return memcmp(s, wanted, wantedLen) == 0 && s[wantedLen] == '\0';
    };

    for (size_t i = 5; i < code.size();) {
        const uint32_t len = code[i] >> 16, op = code[i] & 0xFFFF;
        if (len == 0 || i + len > code.size()) break;
        if (op == kOpCapability && len >= 2 && code[i + 1] == kCapabilityRuntimeDescriptorArray) {
            // The guest's translator emits this because the M19 spoof made it
            // believe runtimeDescriptorArray=true. Mali (M24 raw: 0) refuses to
            // compile a module that declares it, at pipeline creation.
            stripWords.push_back(i);
        } else if (op == kOpExtension && len >= 2 && extensionIs(i, len, "SPV_EXT_descriptor_indexing")) {
            stripWords.push_back(i);
        } else if (op == kOpTypeInt && len >= 4 && code[i + 2] == 32 && code[i + 3] == 0 && !uintTypeId) {
            uintTypeId = code[i + 1];
        } else if (op == kOpConstant && len >= 4) {
            constValue[code[i + 2]] = code[i + 3];
            constType[code[i + 2]] = code[i + 1];
        } else if (len >= 2 && (op == kOpTypeImage || op == kOpTypeSampler || op == kOpTypeSampledImage)) {
            typeOp[code[i + 1]] = op;
        } else if (op == kOpTypeArray && len >= 4) {
            typeOp[code[i + 1]] = op;
            arrayElem[code[i + 1]] = code[i + 2];
            arrayLenConst[code[i + 1]] = code[i + 3];
        } else if (op == kOpTypeRuntimeArray && len >= 3) {
            typeOp[code[i + 1]] = op;
            arrayElem[code[i + 1]] = code[i + 2];
        } else if (op == kOpTypePointer && len >= 4) {
            pointerTo[code[i + 1]] = code[i + 3];
        } else if (op == SpirvRemapper::kOpDecorate && len >= 4) {
            if (code[i + 2] == SpirvRemapper::kDecorationDescriptorSet) setOf[code[i + 1]] = code[i + 3];
            else if (code[i + 2] == SpirvRemapper::kDecorationBinding) bindingOf[code[i + 1]] = code[i + 3];
        } else if (op == SpirvRemapper::kOpVariable && len >= 4) {
            if (code[i + 3] == 0 /*UniformConstant*/) {
                vars.emplace_back(code[i + 2], code[i + 1]);
            } else if (code[i + 3] == 2 /*Uniform*/) {
                ++uniformVarCount;  // UBOs share maxPerStageResources with us
            }
        }
        i += len;
    }

    // Which array types are descriptor arrays, and which (set, binding) use them?
    struct Candidate {
        uint32_t set;
        uint32_t binding;
    };
    std::unordered_map<uint32_t, std::vector<Candidate>> arrayUsers;
    for (const auto& v : vars) {
        const auto ptr = pointerTo.find(v.second);
        if (ptr == pointerTo.end()) continue;
        const uint32_t t = ptr->second;
        const auto tIt = typeOp.find(t);
        if (tIt == typeOp.end() || (tIt->second != kOpTypeArray && tIt->second != kOpTypeRuntimeArray)) continue;
        const auto eIt = arrayElem.find(t);
        if (eIt == arrayElem.end()) continue;
        const auto elemIt = typeOp.find(eIt->second);
        if (elemIt == typeOp.end() || (elemIt->second != kOpTypeImage &&
                                       elemIt->second != kOpTypeSampler &&
                                       elemIt->second != kOpTypeSampledImage)) {
            continue;
        }
        arrayUsers[t].push_back({setOf.count(v.first) ? setOf[v.first] : 0,
                                 bindingOf.count(v.first) ? bindingOf[v.first] : 0});
    }

    struct ArrayEntry {
        uint32_t typeId = 0;
        uint32_t elemOp = 0;
        uint32_t declaredLen = 0;  // 0xFFFFFFFF = runtime, 0xFFFFFFFE = unresolved
        uint32_t finalLen = 0;
    };
    std::vector<ArrayEntry> entries;
    for (const auto& kv : arrayUsers) {
        const uint32_t t = kv.first;
        const auto tIt = typeOp.find(t);
        const auto eIt = arrayElem.find(t);
        if (tIt == typeOp.end() || eIt == arrayElem.end()) continue;
        ArrayEntry e;
        e.typeId = t;
        const auto elemIt = typeOp.find(eIt->second);
        e.elemOp = elemIt != typeOp.end() ? elemIt->second : 0;
        const bool isSampler = e.elemOp == kOpTypeSampler;
        uint32_t limit = isSampler ? 128 : 256;
        for (const auto& c : kv.second) {
            uint32_t mapped = 0;
            if (c.set == 0 && c.binding == 0) mapped = 256;
            else if (c.set == 1 && c.binding == 0) mapped = 16;
            else if (c.set == 1 && c.binding == 1) mapped = 64;
            else if (c.set == 2 && c.binding == 0) mapped = 128;
            if (mapped && mapped < limit) limit = mapped;
        }
        if (tIt->second == kOpTypeRuntimeArray) {
            e.declaredLen = 0xFFFFFFFFu;
            e.finalLen = limit;
        } else {
            uint32_t declared = 0xFFFFFFFEu;
            const auto lc = arrayLenConst.find(t);
            if (lc != arrayLenConst.end()) {
                const auto cv = constValue.find(lc->second);
                if (cv != constValue.end()) declared = cv->second;
            }
            e.declaredLen = declared;
            e.finalLen = (declared != 0xFFFFFFFEu && declared < limit) ? declared : limit;
        }
        entries.push_back(e);
    }

    // Budget the stage: per-type sums and the total, keeping a small margin under
    // maxPerStageResources (361) since UBOs and other resources share it.
    auto sumByKind = [&](bool samplerKind) {
        uint32_t s = 0;
        for (const auto& e : entries) {
            if ((e.elemOp == kOpTypeSampler) == samplerKind) s += e.finalLen;
        }
        return s;
    };
    auto shrinkLargest = [&](bool samplerKind, uint32_t excess, uint32_t floor) {
        while (excess > 0) {
            ArrayEntry* biggest = nullptr;
            for (auto& e : entries) {
                if ((e.elemOp == kOpTypeSampler) != samplerKind) continue;
                if (e.finalLen <= floor) continue;
                if (!biggest || e.finalLen > biggest->finalLen) biggest = &e;
            }
            if (!biggest) break;
            uint32_t take = excess;
            const uint32_t room = biggest->finalLen - floor;
            if (take > room) take = room;
            biggest->finalLen -= take;
            excess -= take;
        }
    };
    // maxPerStageResources (361) counts every descriptor the stage can reach:
    // images, samplers and the UBOs too, so keep a few slots of margin.
    const uint32_t otherResources = uniformVarCount + 4;
    const uint32_t targetTotal = otherResources < 300 ? 361 - otherResources : 128;
    if (sumByKind(true) > 128) shrinkLargest(true, sumByKind(true) - 128, 32);
    if (sumByKind(false) > 256) shrinkLargest(false, sumByKind(false) - 256, 48);
    // Prefer shaving the deduplicated sampler heap further before touching images.
    uint32_t total = sumByKind(true) + sumByKind(false);
    if (total > targetTotal) {
        const uint32_t before = sumByKind(true);
        shrinkLargest(true, total - targetTotal, 32);
        total -= (before - sumByKind(true));
    }
    if (total > targetTotal) {
        shrinkLargest(false, total - targetTotal, 48);
        total = sumByKind(true) + sumByKind(false);
    }
    if (total > targetTotal) {
        LOGW("B10: descriptor arrays still total %u resources against a %u budget; pipelines may be rejected",
             total, targetTotal);
    }

    uint32_t nextId = code[3];
    std::vector<uint32_t> pending;  // new type/constant instructions
    auto ensureLengthConstant = [&](uint32_t value) -> uint32_t {
        if (!uintTypeId) {
            uintTypeId = nextId++;
            pending.push_back((2u << 16) | kOpTypeInt);
            pending.push_back(uintTypeId);
            pending.push_back(32);
            pending.push_back(0);
        }
        for (const auto& kv : constValue) {
            if (kv.second == value && constType.count(kv.first) && constType[kv.first] == uintTypeId) {
                return kv.first;
            }
        }
        const uint32_t cid = nextId++;
        pending.push_back((4u << 16) | kOpConstant);
        pending.push_back(uintTypeId);
        pending.push_back(cid);
        pending.push_back(value);
        return cid;
    };

    std::unordered_map<uint32_t, uint32_t> newLenConst;  // array type id -> constant id
    std::unordered_map<uint32_t, bool> makeFixed;        // runtime array type id -> convert
    uint32_t changes = 0;
    for (auto& e : entries) {
        if (e.declaredLen == 0xFFFFFFFEu) continue;  // length is a spec constant: leave it alone
        if (e.finalLen == e.declaredLen) continue;   // fixed and already within the budget
        newLenConst[e.typeId] = ensureLengthConstant(e.finalLen);
        makeFixed[e.typeId] = (e.declaredLen == 0xFFFFFFFFu);
        ++changes;
        LOGI("B10: array type #%u %s -> fixed length %u", e.typeId,
             e.declaredLen == 0xFFFFFFFFu ? "(runtime array)" : "(oversized)", e.finalLen);
    }

    // Drop the descriptor-indexing declarations as well: with every descriptor
    // runtime array now a fixed array, nothing needs the capability, and Mali
    // compiles the module at pipeline creation, where it rejects a capability the
    // device does not have (this is the -3 that survived the array rewrite).
    // Keep them when some runtime array of descriptors would survive the pass, so
    // the module stays coherent.
    bool stripDescriptorIndexing = !stripWords.empty();
    if (stripDescriptorIndexing) {
        for (const auto& kv : typeOp) {
            if (kv.second != kOpTypeRuntimeArray || makeFixed.count(kv.first)) continue;
            const auto eIt = arrayElem.find(kv.first);
            if (eIt == arrayElem.end()) continue;
            const auto elemIt = typeOp.find(eIt->second);
            if (elemIt == typeOp.end()) continue;
            if (elemIt->second == kOpTypeImage || elemIt->second == kOpTypeSampler ||
                elemIt->second == kOpTypeSampledImage) {
                stripDescriptorIndexing = false;  // a descriptor runtime array remains
                break;
            }
        }
    }
    if (!changes && !stripDescriptorIndexing) return 0;

    // Rebuild the module: swap the array instructions and insert the new
    // type/constant instructions immediately before the first rewrite, because
    // SPIR-V requires every definition to precede its first use (the rewritten
    // array types are the first users of the new constants).
    std::vector<uint32_t> out;
    out.reserve(code.size() + pending.size() + 8);
    out.insert(out.end(), code.begin(), code.begin() + 5);
    bool pendingPlaced = pending.empty();
    size_t stripIdx = 0;
    for (size_t i = 5; i < code.size();) {
        const uint32_t len = code[i] >> 16, op = code[i] & 0xFFFF;
        if (len == 0 || i + len > code.size()) {
            if (!pendingPlaced) {
                out.insert(out.end(), pending.begin(), pending.end());
                pendingPlaced = true;
            }
            out.insert(out.end(), code.begin() + i, code.end());
            break;
        }
        while (stripIdx < stripWords.size() && stripWords[stripIdx] < i) ++stripIdx;
        if (stripDescriptorIndexing && stripIdx < stripWords.size() && stripWords[stripIdx] == i) {
            i += len;  // drop the declaration
            continue;
        }
        const uint32_t idAt = (len >= 2 && (op == kOpTypeArray || op == kOpTypeRuntimeArray)) ? code[i + 1] : 0;
        const auto mk = makeFixed.find(idAt);
        const bool runtimeToFixed = (mk != makeFixed.end() && mk->second && op == kOpTypeRuntimeArray);
        const bool resizeArray = (op == kOpTypeArray && newLenConst.count(idAt) != 0);
        if (!pendingPlaced && (runtimeToFixed || resizeArray)) {
            out.insert(out.end(), pending.begin(), pending.end());
            pendingPlaced = true;
        }
        if (runtimeToFixed) {
            out.push_back((4u << 16) | kOpTypeArray);
            out.push_back(code[i + 1]);
            out.push_back(code[i + 2]);
            out.push_back(newLenConst[idAt]);
        } else if (resizeArray) {
            out.insert(out.end(), code.begin() + i, code.begin() + i + len);
            out.back() = newLenConst[idAt];  // OpTypeArray's last word is the length constant
        } else {
            out.insert(out.end(), code.begin() + i, code.begin() + i + len);
        }
        i += len;
    }
    if (!pendingPlaced) out.insert(out.end(), pending.begin(), pending.end());
    out[3] = nextId;  // raise the id bound for the instructions we added
    code.swap(out);
    if (changes) {
        LOGI("B10: clamped %u descriptor array type(s) in this module", changes);
    }
    if (stripDescriptorIndexing) {
        LOGI("B10: stripped the descriptor-indexing declarations (OpCapability RuntimeDescriptorArray + SPV_EXT_descriptor_indexing)");
    }
    return changes + (stripDescriptorIndexing ? 1u : 0u);
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

    // B10: the guest's own module is kept except for descriptor array lengths,
    // which are rewritten to the fixed, budget-sized shapes the Mali driver
    // accepts (see B10ClampDescriptorArrays; without this every geometry
    // pipeline with a runtime-array heap dies with VK_ERROR_INITIALIZATION_FAILED).
    B10ClampDescriptorArrays(tempCode);

    // Log the descriptor set inventory so we can see, from the host side, exactly
    // which sets the guest produced instead of having to infer it from the game's
    // own log. Shader creation is rare, so this is not worth gating behind a debug
    // level.
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
