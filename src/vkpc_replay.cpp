// vkpc_replay: replays a pipeline capture (.vkpc) without the application that recorded it.
//
// A capture holds the device setup (extensions, feature structs) and every object pipeline
// creation depends on: shader modules, samplers, descriptor set layouts, pipeline layouts, render
// passes, pipeline caches (with their seed data), cache merges, and each graphics/compute pipeline
// with its full state and the recording thread. See FORMAT.md.
//
// Usage: vkpc_replay <file.vkpc> [options]
//   --list                 print one line per pipeline and exit
//   --only N[,N...]        create only these pipelines (all other objects are still created)
//   --from N  --to N       create only pipelines in [from, to]
//   --threads T            create the selected pipelines from T threads at once (round robin)
//   --no-seed              ignore recorded pipeline-cache seed data (caches start empty)
//   --no-cache             pass VK_NULL_HANDLE as the pipeline cache
//   --dump-spv N DIR       write the SPIR-V of pipeline N's stages to DIR/<stage>.spv and exit
//   --spv STAGE=FILE       with --only N: replace that stage's SPIR-V (vert, frag, comp, geom,
//                          tesc, tese) with FILE, e.g. a spirv-reduce candidate
// Each pipeline prints "pipeline N ..." before the driver call (stdout is unbuffered), so after a
// driver fault the last line names the faulting pipeline. Exit 0 = every call returned VK_SUCCESS.
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

[[noreturn]] void Die(const char* what) {
    std::printf("FATAL: %s\n", what);
    std::exit(2);
}

#define CHECK(call)                                                                       \
    do {                                                                                  \
        VkResult r_ = (call);                                                             \
        if (r_ != VK_SUCCESS) {                                                           \
            std::printf("FAIL %s -> VkResult %d (line %d)\n", #call, r_, __LINE__);       \
            std::exit(2);                                                                 \
        }                                                                                 \
    } while (0)

constexpr size_t kHeader = sizeof(VkBaseInStructure);

enum Tag : uint32_t {
    TagDevice = 1, TagShader = 2, TagSetLayout = 3, TagPipelineLayout = 4, TagRenderPass = 5,
    TagCache = 6, TagMerge = 7, TagGraphics = 8, TagSampler = 9, TagCompute = 10, TagResult = 11,
};

// ---- reading --------------------------------------------------------------------------------

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    void Need(size_t n) const {
        if (size_t(end - p) < n) Die("truncated record");
    }
    void Bytes(void* out, size_t n) {
        Need(n);
        std::memcpy(out, p, n);
        p += n;
    }
    uint32_t U32() { uint32_t v; Bytes(&v, 4); return v; }
    int32_t I32() { int32_t v; Bytes(&v, 4); return v; }
    uint64_t U64() { uint64_t v; Bytes(&v, 8); return v; }
    float F32() { float v; Bytes(&v, 4); return v; }
    std::string Str() {
        const uint32_t n = U32();
        Need(n);
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    std::vector<uint8_t> Blob() {
        const uint64_t n = U64();
        Need(n);
        std::vector<uint8_t> v(p, p + n);
        p += n;
        return v;
    }
    template <typename T>
    T Pod() { T v; Bytes(&v, sizeof(T)); return v; }
};

// A pNext chain rebuilt in owned memory.
struct Chain {
    std::vector<std::unique_ptr<uint8_t[]>> nodes;
    std::vector<std::vector<VkVertexInputBindingDivisorDescriptionEXT>> divisors;
    std::vector<std::vector<VkDescriptorBindingFlags>> flags;
    const void* head = nullptr;

    void Read(Reader& r) {
        const uint32_t count = r.U32();
        std::vector<VkBaseOutStructure*> list;
        for (uint32_t i = 0; i < count; ++i) {
            const auto stype = VkStructureType(r.U32());
            const uint32_t kind = r.U32();
            if (kind == 0) {
                std::vector<uint8_t> body = r.Blob();
                auto mem = std::make_unique<uint8_t[]>(kHeader + body.size());
                std::memset(mem.get(), 0, kHeader);
                std::memcpy(mem.get() + kHeader, body.data(), body.size());
                reinterpret_cast<VkBaseOutStructure*>(mem.get())->sType = stype;
                list.push_back(reinterpret_cast<VkBaseOutStructure*>(mem.get()));
                nodes.push_back(std::move(mem));
            } else if (kind == 1) {
                auto& d = divisors.emplace_back(r.U32());
                for (auto& e : d) { e.binding = r.U32(); e.divisor = r.U32(); }
                auto mem = std::make_unique<uint8_t[]>(sizeof(VkPipelineVertexInputDivisorStateCreateInfoEXT));
                auto* s = reinterpret_cast<VkPipelineVertexInputDivisorStateCreateInfoEXT*>(mem.get());
                *s = {stype, nullptr, uint32_t(d.size()), d.data()};
                list.push_back(reinterpret_cast<VkBaseOutStructure*>(mem.get()));
                nodes.push_back(std::move(mem));
            } else if (kind == 2) {
                auto& f = flags.emplace_back(r.U32());
                for (auto& e : f) e = r.U32();
                auto mem = std::make_unique<uint8_t[]>(sizeof(VkDescriptorSetLayoutBindingFlagsCreateInfo));
                auto* s = reinterpret_cast<VkDescriptorSetLayoutBindingFlagsCreateInfo*>(mem.get());
                *s = {stype, nullptr, uint32_t(f.size()), f.data()};
                list.push_back(reinterpret_cast<VkBaseOutStructure*>(mem.get()));
                nodes.push_back(std::move(mem));
            } else {
                std::printf("warning: pNext sType %d was not recorded; replaying without it\n", int(stype));
            }
        }
        for (size_t i = 0; i + 1 < list.size(); ++i) list[i]->pNext = list[i + 1];
        head = list.empty() ? nullptr : list[0];
    }
};

// ---- state ----------------------------------------------------------------------------------

struct Options {
    std::set<uint32_t> only;
    int64_t from = -1, to = -1;
    int threads = 0;
    bool no_seed = false, no_cache = false, list = false;
    int64_t dump_index = -1;
    std::string dump_dir;
    std::map<uint32_t, std::string> spv_override; // stage bit -> file
};

struct Replay {
    Options opt;
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    std::map<uint64_t, VkShaderModule> shaders;
    std::map<uint64_t, std::vector<uint8_t>> shader_code;
    std::map<uint64_t, VkSampler> samplers;
    std::map<uint64_t, VkDescriptorSetLayout> set_layouts;
    std::map<uint64_t, VkPipelineLayout> pipeline_layouts;
    std::map<uint64_t, VkRenderPass> render_passes;
    std::map<uint64_t, VkPipelineCache> caches;
    std::map<uint32_t, int32_t> recorded_result;
    std::atomic<int> failures{0};

    template <typename H>
    static H Find(const std::map<uint64_t, H>& m, uint64_t old, const char* what) {
        if (old == 0) return VK_NULL_HANDLE;
        auto it = m.find(old);
        if (it == m.end()) {
            std::printf("FATAL: %s 0x%llx not in capture\n", what, (unsigned long long)old);
            std::exit(2);
        }
        return it->second;
    }
};

const char* StageName(uint32_t bit) {
    switch (bit) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vert";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "frag";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "comp";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "geom";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tesc";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tese";
    default: return "stage";
    }
}

uint32_t StageBit(const std::string& name) {
    for (uint32_t bit : {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                         VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                         VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                         VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT})
        if (name == StageName(bit)) return bit;
    Die("unknown stage name");
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) Die(("cannot read " + path).c_str());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// ---- pipeline jobs --------------------------------------------------------------------------

struct StageData {
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    std::string name;
    uint64_t old_module = 0;
    bool has_spec = false;
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<uint8_t> spec_data;
    VkSpecializationInfo spec{};
    Chain chain;
    VkShaderModule own_module = VK_NULL_HANDLE; // created for --spv overrides
};

struct Job {
    uint32_t index = 0;
    uint32_t thread = 0;
    bool compute = false;
    uint64_t old_cache = 0;
    std::vector<std::unique_ptr<StageData>> stages;
    std::vector<VkPipelineShaderStageCreateInfo> stage_infos;
    Chain chain, vi_chain, vp_chain, rs_chain;
    VkGraphicsPipelineCreateInfo gfx{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkComputePipelineCreateInfo comp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    std::vector<VkSampleMask> sample_mask;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    std::vector<VkPipelineColorBlendAttachmentState> blend;
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    std::vector<VkDynamicState> dynamic;
    bool has_vi = false, has_ia = false, has_ts = false, has_vp = false, has_rs = false,
         has_ms = false, has_ds = false, has_cb = false, has_dy = false;
};

std::unique_ptr<StageData> ReadStage(Reader& r) {
    auto s = std::make_unique<StageData>();
    s->info.flags = r.U32();
    s->info.stage = VkShaderStageFlagBits(r.U32());
    s->old_module = r.U64();
    s->name = r.Str();
    s->has_spec = r.U32() != 0;
    if (s->has_spec) {
        s->entries.resize(r.U32());
        for (auto& e : s->entries) {
            e.constantID = r.U32();
            e.offset = r.U32();
            e.size = size_t(r.U64());
        }
        s->spec_data = r.Blob();
    }
    s->chain.Read(r);
    return s;
}

void Body(Reader& r, void* s, size_t size) {
    std::vector<uint8_t> body = r.Blob();
    if (body.size() != size - kHeader) Die("struct size mismatch");
    std::memcpy(static_cast<uint8_t*>(s) + kHeader, body.data(), body.size());
}

std::unique_ptr<Job> ReadGraphics(Reader& r) {
    auto j = std::make_unique<Job>();
    j->index = r.U32();
    j->old_cache = r.U64();
    j->gfx.flags = r.U32();
    j->gfx.layout = reinterpret_cast<VkPipelineLayout>(r.U64()); // old handle, resolved later
    j->gfx.renderPass = reinterpret_cast<VkRenderPass>(r.U64());
    j->gfx.subpass = r.U32();
    j->chain.Read(r);
    const uint32_t stages = r.U32();
    for (uint32_t i = 0; i < stages; ++i) j->stages.push_back(ReadStage(r));
    if ((j->has_vi = r.U32())) {
        j->vi.flags = r.U32();
        j->vi_chain.Read(r);
        j->bindings.resize(r.U32());
        for (auto& b : j->bindings) b = r.Pod<VkVertexInputBindingDescription>();
        j->attributes.resize(r.U32());
        for (auto& a : j->attributes) a = r.Pod<VkVertexInputAttributeDescription>();
    }
    if ((j->has_ia = r.U32())) {
        j->ia.flags = r.U32();
        j->ia.topology = VkPrimitiveTopology(r.U32());
        j->ia.primitiveRestartEnable = r.U32();
    }
    if ((j->has_ts = r.U32())) {
        j->ts.flags = r.U32();
        j->ts.patchControlPoints = r.U32();
    }
    if ((j->has_vp = r.U32())) {
        j->vp.flags = r.U32();
        j->vp_chain.Read(r);
        j->vp.viewportCount = r.U32();
        if (r.U32()) {
            j->viewports.resize(j->vp.viewportCount);
            for (auto& v : j->viewports) v = r.Pod<VkViewport>();
        }
        j->vp.scissorCount = r.U32();
        if (r.U32()) {
            j->scissors.resize(j->vp.scissorCount);
            for (auto& s : j->scissors) s = r.Pod<VkRect2D>();
        }
    }
    if ((j->has_rs = r.U32())) {
        Body(r, &j->rs, sizeof(j->rs));
        j->rs_chain.Read(r);
    }
    if ((j->has_ms = r.U32())) {
        j->ms.flags = r.U32();
        j->ms.rasterizationSamples = VkSampleCountFlagBits(r.U32());
        j->ms.sampleShadingEnable = r.U32();
        j->ms.minSampleShading = r.F32();
        j->sample_mask.resize(r.U32());
        for (auto& m : j->sample_mask) m = r.U32();
        j->ms.alphaToCoverageEnable = r.U32();
        j->ms.alphaToOneEnable = r.U32();
    }
    if ((j->has_ds = r.U32())) Body(r, &j->ds, sizeof(j->ds));
    if ((j->has_cb = r.U32())) {
        j->cb.flags = r.U32();
        j->cb.logicOpEnable = r.U32();
        j->cb.logicOp = VkLogicOp(r.U32());
        j->cb.attachmentCount = r.U32();
        if (r.U32()) {
            j->blend.resize(j->cb.attachmentCount);
            for (auto& b : j->blend) b = r.Pod<VkPipelineColorBlendAttachmentState>();
        }
        for (float& c : j->cb.blendConstants) c = r.F32();
    }
    if ((j->has_dy = r.U32())) {
        j->dynamic.resize(r.U32());
        for (auto& d : j->dynamic) d = VkDynamicState(r.U32());
    }
    return j;
}

std::unique_ptr<Job> ReadCompute(Reader& r) {
    auto j = std::make_unique<Job>();
    j->compute = true;
    j->index = r.U32();
    j->old_cache = r.U64();
    j->comp.flags = r.U32();
    j->comp.layout = reinterpret_cast<VkPipelineLayout>(r.U64());
    j->chain.Read(r);
    j->stages.push_back(ReadStage(r));
    return j;
}

// Resolve old handles and wire the pointers; after this the job is ready for the driver.
void Wire(Replay& rp, Job& j) {
    for (auto& s : j.stages) {
        auto it = rp.opt.spv_override.find(s->info.stage);
        if (it != rp.opt.spv_override.end()) {
            std::vector<uint8_t> code = ReadFile(it->second);
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = code.size();
            ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
            CHECK(vkCreateShaderModule(rp.device, &ci, nullptr, &s->own_module));
            s->info.module = s->own_module;
            std::printf("  %s stage replaced by %s (%zu bytes)\n", StageName(s->info.stage),
                        it->second.c_str(), code.size());
        } else {
            s->info.module = Replay::Find(rp.shaders, s->old_module, "shader module");
        }
        s->info.pName = s->name.c_str();
        s->info.pNext = s->chain.head;
        if (s->has_spec) {
            s->spec = {uint32_t(s->entries.size()), s->entries.data(), s->spec_data.size(),
                       s->spec_data.data()};
            s->info.pSpecializationInfo = &s->spec;
        }
        j.stage_infos.push_back(s->info);
    }
    if (j.compute) {
        j.comp.pNext = j.chain.head;
        j.comp.stage = j.stage_infos[0];
        j.comp.layout = Replay::Find(rp.pipeline_layouts, reinterpret_cast<uint64_t>(j.comp.layout),
                                     "pipeline layout");
        return;
    }
    VkGraphicsPipelineCreateInfo& g = j.gfx;
    g.pNext = j.chain.head;
    g.stageCount = uint32_t(j.stage_infos.size());
    g.pStages = j.stage_infos.data();
    g.layout = Replay::Find(rp.pipeline_layouts, reinterpret_cast<uint64_t>(g.layout), "pipeline layout");
    g.renderPass = Replay::Find(rp.render_passes, reinterpret_cast<uint64_t>(g.renderPass), "render pass");
    if (j.has_vi) {
        j.vi.pNext = j.vi_chain.head;
        j.vi.vertexBindingDescriptionCount = uint32_t(j.bindings.size());
        j.vi.pVertexBindingDescriptions = j.bindings.data();
        j.vi.vertexAttributeDescriptionCount = uint32_t(j.attributes.size());
        j.vi.pVertexAttributeDescriptions = j.attributes.data();
        g.pVertexInputState = &j.vi;
    }
    if (j.has_ia) g.pInputAssemblyState = &j.ia;
    if (j.has_ts) g.pTessellationState = &j.ts;
    if (j.has_vp) {
        j.vp.pNext = j.vp_chain.head;
        j.vp.pViewports = j.viewports.empty() ? nullptr : j.viewports.data();
        j.vp.pScissors = j.scissors.empty() ? nullptr : j.scissors.data();
        g.pViewportState = &j.vp;
    }
    if (j.has_rs) {
        j.rs.pNext = j.rs_chain.head;
        g.pRasterizationState = &j.rs;
    }
    if (j.has_ms) {
        j.ms.pSampleMask = j.sample_mask.empty() ? nullptr : j.sample_mask.data();
        g.pMultisampleState = &j.ms;
    }
    if (j.has_ds) g.pDepthStencilState = &j.ds;
    if (j.has_cb) {
        j.cb.pAttachments = j.blend.empty() ? nullptr : j.blend.data();
        g.pColorBlendState = &j.cb;
    }
    if (j.has_dy) {
        j.dy.dynamicStateCount = uint32_t(j.dynamic.size());
        j.dy.pDynamicStates = j.dynamic.data();
        g.pDynamicState = &j.dy;
    }
}

void Run(Replay& rp, Job& j) {
    VkPipelineCache cache = rp.opt.no_cache ? VK_NULL_HANDLE
                                            : Replay::Find(rp.caches, j.old_cache, "pipeline cache");
    std::printf("pipeline %u (%s, %zu stages, recorded thread %u, cache %s)...\n", j.index,
                j.compute ? "compute" : "graphics", j.stages.size(), j.thread,
                cache ? "yes" : "none");
    VkPipeline out{};
    const VkResult r = j.compute
                           ? vkCreateComputePipelines(rp.device, cache, 1, &j.comp, nullptr, &out)
                           : vkCreateGraphicsPipelines(rp.device, cache, 1, &j.gfx, nullptr, &out);
    std::printf("pipeline %u -> %d\n", j.index, int(r));
    if (r != VK_SUCCESS) ++rp.failures;
    if (out) vkDestroyPipeline(rp.device, out, nullptr);
}

bool Selected(const Options& o, uint32_t index) {
    if (!o.only.empty() && !o.only.count(index)) return false;
    if (o.from >= 0 && int64_t(index) < o.from) return false;
    if (o.to >= 0 && int64_t(index) > o.to) return false;
    return true;
}

// ---- object records -------------------------------------------------------------------------

void CreateDevice(Replay& rp, Reader& r) {
    const uint32_t api = r.U32(), vendor = r.U32(), device_id = r.U32(), driver = r.U32();
    const std::string name = r.Str();
    std::vector<std::string> exts(r.U32());
    for (auto& e : exts) e = r.Str();
    const bool has_features = r.U32() != 0;
    VkPhysicalDeviceFeatures features{};
    if (has_features) features = r.Pod<VkPhysicalDeviceFeatures>();
    std::vector<std::unique_ptr<uint8_t[]>> nodes;
    std::vector<VkBaseOutStructure*> list;
    const uint32_t count = r.U32();
    for (uint32_t i = 0; i < count; ++i) {
        const auto stype = VkStructureType(r.U32());
        std::vector<uint8_t> body = r.Blob();
        if (body.empty()) continue;
        auto mem = std::make_unique<uint8_t[]>(kHeader + body.size());
        std::memset(mem.get(), 0, kHeader);
        std::memcpy(mem.get() + kHeader, body.data(), body.size());
        reinterpret_cast<VkBaseOutStructure*>(mem.get())->sType = stype;
        list.push_back(reinterpret_cast<VkBaseOutStructure*>(mem.get()));
        nodes.push_back(std::move(mem));
    }
    for (size_t i = 0; i + 1 < list.size(); ++i) list[i]->pNext = list[i + 1];

    std::printf("capture: %s api %u.%u.%u vendor 0x%04x device 0x%08x driver 0x%08x, %zu extensions, %zu feature structs\n",
                name.c_str(), VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), VK_VERSION_PATCH(api),
                vendor, device_id, driver, exts.size(), list.size());

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkpc_replay";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, nullptr, &rp.instance));
    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(rp.instance, &n, nullptr));
    if (!n) Die("no Vulkan device");
    std::vector<VkPhysicalDevice> gpus(n);
    CHECK(vkEnumeratePhysicalDevices(rp.instance, &n, gpus.data()));
    rp.gpu = gpus[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(rp.gpu, &props);
    std::printf("device:  %s api %u.%u.%u vendor 0x%04x device 0x%08x driver 0x%08x\n",
                props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
                props.vendorID, props.deviceID, props.driverVersion);
    if (props.vendorID != vendor || props.deviceID != device_id || props.driverVersion != driver)
        std::printf("warning: replaying on a different device or driver than the capture\n");

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(rp.gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(rp.gpu, &qn, qf.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<const char*> ext_ptrs;
    for (auto& e : exts) ext_ptrs.push_back(e.c_str());
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = list.empty() ? nullptr : list[0];
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(ext_ptrs.size());
    dci.ppEnabledExtensionNames = ext_ptrs.data();
    dci.pEnabledFeatures = has_features ? &features : nullptr;
    CHECK(vkCreateDevice(rp.gpu, &dci, nullptr, &rp.device));
}

void CreateShader(Replay& rp, Reader& r) {
    const uint64_t old = r.U64();
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.flags = r.U32();
    std::vector<uint8_t> code = r.Blob();
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m{};
    CHECK(vkCreateShaderModule(rp.device, &ci, nullptr, &m));
    rp.shaders[old] = m;
    rp.shader_code[old] = std::move(code);
}

void CreateSampler(Replay& rp, Reader& r) {
    const uint64_t old = r.U64();
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    Body(r, &ci, sizeof(ci));
    Chain chain;
    chain.Read(r);
    ci.pNext = chain.head;
    VkSampler s{};
    CHECK(vkCreateSampler(rp.device, &ci, nullptr, &s));
    rp.samplers[old] = s;
}

void CreateSetLayout(Replay& rp, Reader& r) {
    const uint64_t old = r.U64();
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.flags = r.U32();
    Chain chain;
    chain.Read(r);
    ci.pNext = chain.head;
    std::vector<VkDescriptorSetLayoutBinding> bindings(r.U32());
    std::vector<std::vector<VkSampler>> immutable(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        auto& b = bindings[i];
        b.binding = r.U32();
        b.descriptorType = VkDescriptorType(r.U32());
        b.descriptorCount = r.U32();
        b.stageFlags = r.U32();
        immutable[i].resize(r.U32());
        for (auto& s : immutable[i]) s = Replay::Find(rp.samplers, r.U64(), "sampler");
        b.pImmutableSamplers = immutable[i].empty() ? nullptr : immutable[i].data();
    }
    ci.bindingCount = uint32_t(bindings.size());
    ci.pBindings = bindings.data();
    VkDescriptorSetLayout l{};
    CHECK(vkCreateDescriptorSetLayout(rp.device, &ci, nullptr, &l));
    rp.set_layouts[old] = l;
}

void CreatePipelineLayout(Replay& rp, Reader& r) {
    const uint64_t old = r.U64();
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.flags = r.U32();
    std::vector<VkDescriptorSetLayout> sets(r.U32());
    for (auto& s : sets) s = Replay::Find(rp.set_layouts, r.U64(), "set layout");
    std::vector<VkPushConstantRange> push(r.U32());
    for (auto& p : push) p = r.Pod<VkPushConstantRange>();
    ci.setLayoutCount = uint32_t(sets.size());
    ci.pSetLayouts = sets.data();
    ci.pushConstantRangeCount = uint32_t(push.size());
    ci.pPushConstantRanges = push.data();
    VkPipelineLayout l{};
    CHECK(vkCreatePipelineLayout(rp.device, &ci, nullptr, &l));
    rp.pipeline_layouts[old] = l;
}

void CreateRenderPass(Replay& rp, Reader& r) {
    const uint64_t old = r.U64();
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.flags = r.U32();
    Chain chain;
    chain.Read(r);
    ci.pNext = chain.head;
    std::vector<VkAttachmentDescription> atts(r.U32());
    for (auto& a : atts) a = r.Pod<VkAttachmentDescription>();
    struct Sub {
        std::vector<VkAttachmentReference> input, color, resolve;
        bool has_depth = false;
        VkAttachmentReference depth{};
        std::vector<uint32_t> preserve;
    };
    std::vector<VkSubpassDescription> subs(r.U32());
    std::vector<Sub> data(subs.size());
    for (size_t i = 0; i < subs.size(); ++i) {
        Sub& d = data[i];
        subs[i].flags = r.U32();
        subs[i].pipelineBindPoint = VkPipelineBindPoint(r.U32());
        d.input.resize(r.U32());
        for (auto& x : d.input) x = r.Pod<VkAttachmentReference>();
        d.color.resize(r.U32());
        for (auto& x : d.color) x = r.Pod<VkAttachmentReference>();
        if (r.U32()) {
            d.resolve.resize(d.color.size());
            for (auto& x : d.resolve) x = r.Pod<VkAttachmentReference>();
        }
        if ((d.has_depth = r.U32())) d.depth = r.Pod<VkAttachmentReference>();
        d.preserve.resize(r.U32());
        for (auto& x : d.preserve) x = r.U32();
        subs[i].inputAttachmentCount = uint32_t(d.input.size());
        subs[i].pInputAttachments = d.input.empty() ? nullptr : d.input.data();
        subs[i].colorAttachmentCount = uint32_t(d.color.size());
        subs[i].pColorAttachments = d.color.empty() ? nullptr : d.color.data();
        subs[i].pResolveAttachments = d.resolve.empty() ? nullptr : d.resolve.data();
        subs[i].pDepthStencilAttachment = d.has_depth ? &d.depth : nullptr;
        subs[i].preserveAttachmentCount = uint32_t(d.preserve.size());
        subs[i].pPreserveAttachments = d.preserve.empty() ? nullptr : d.preserve.data();
    }
    std::vector<VkSubpassDependency> deps(r.U32());
    for (auto& d : deps) d = r.Pod<VkSubpassDependency>();
    ci.attachmentCount = uint32_t(atts.size());
    ci.pAttachments = atts.data();
    ci.subpassCount = uint32_t(subs.size());
    ci.pSubpasses = subs.data();
    ci.dependencyCount = uint32_t(deps.size());
    ci.pDependencies = deps.data();
    VkRenderPass p{};
    CHECK(vkCreateRenderPass(rp.device, &ci, nullptr, &p));
    rp.render_passes[old] = p;
}

void CreateCache(Replay& rp, Reader& r) {
    const uint64_t old = r.U64();
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.flags = r.U32();
    std::vector<uint8_t> seed = r.Blob();
    if (!rp.opt.no_seed && !seed.empty()) {
        ci.initialDataSize = seed.size();
        ci.pInitialData = seed.data();
    }
    VkPipelineCache c{};
    CHECK(vkCreatePipelineCache(rp.device, &ci, nullptr, &c));
    std::printf("pipeline cache created, seed %zu bytes%s\n", seed.size(),
                (rp.opt.no_seed && !seed.empty()) ? " (ignored: --no-seed)" : "");
    rp.caches[old] = c;
}

void Merge(Replay& rp, Reader& r) {
    VkPipelineCache dst = Replay::Find(rp.caches, r.U64(), "pipeline cache");
    std::vector<VkPipelineCache> src(r.U32());
    for (auto& s : src) s = Replay::Find(rp.caches, r.U64(), "pipeline cache");
    if (rp.opt.no_cache) return;
    std::printf("merging %zu caches\n", src.size());
    CHECK(vkMergePipelineCaches(rp.device, dst, uint32_t(src.size()), src.data()));
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: %s <file.vkpc> [--list] [--only N,..] [--from N] [--to N] [--threads T]\n"
                    "       [--no-seed] [--no-cache] [--dump-spv N DIR] [--spv STAGE=FILE]\n", argv[0]);
        return 1;
    }
    Replay rp;
    Options& o = rp.opt;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) Die("missing option value");
            return argv[++i];
        };
        if (a == "--list") o.list = true;
        else if (a == "--only") {
            std::string v = next();
            for (size_t p = 0; p < v.size();) {
                size_t q = v.find(',', p);
                o.only.insert(uint32_t(std::stoul(v.substr(p, q - p))));
                p = q == std::string::npos ? v.size() : q + 1;
            }
        } else if (a == "--from") o.from = std::stoll(next());
        else if (a == "--to") o.to = std::stoll(next());
        else if (a == "--threads") o.threads = std::stoi(next());
        else if (a == "--no-seed") o.no_seed = true;
        else if (a == "--no-cache") o.no_cache = true;
        else if (a == "--dump-spv") { o.dump_index = std::stoll(next()); o.dump_dir = next(); }
        else if (a == "--spv") {
            std::string v = next();
            size_t eq = v.find('=');
            if (eq == std::string::npos) Die("--spv wants STAGE=FILE");
            o.spv_override[StageBit(v.substr(0, eq))] = v.substr(eq + 1);
        } else Die(("unknown option " + a).c_str());
    }

    std::vector<uint8_t> file = ReadFile(argv[1]);
    if (file.size() < 8 || std::memcmp(file.data(), "VKPC", 4) != 0) Die("not a .vkpc file");
    Reader r{file.data() + 8, file.data() + file.size()};

    std::vector<std::unique_ptr<Job>> deferred;
    while (r.p < r.end) {
        const uint32_t tag = r.U32();
        const uint32_t thread = r.U32();
        const uint64_t size = r.U64();
        r.Need(size);
        Reader rec{r.p, r.p + size};
        r.p += size;
        if (tag == TagResult) {
            const uint32_t index = rec.U32();
            rp.recorded_result[index] = rec.I32();
            continue;
        }
        if (o.list && tag != TagGraphics && tag != TagCompute && tag != TagDevice) continue;
        switch (tag) {
        case TagDevice:
            if (o.list) break;
            CreateDevice(rp, rec);
            break;
        case TagShader: CreateShader(rp, rec); break;
        case TagSampler: CreateSampler(rp, rec); break;
        case TagSetLayout: CreateSetLayout(rp, rec); break;
        case TagPipelineLayout: CreatePipelineLayout(rp, rec); break;
        case TagRenderPass: CreateRenderPass(rp, rec); break;
        case TagCache: CreateCache(rp, rec); break;
        case TagMerge: Merge(rp, rec); break;
        case TagGraphics:
        case TagCompute: {
            std::unique_ptr<Job> j = tag == TagGraphics ? ReadGraphics(rec) : ReadCompute(rec);
            j->thread = thread;
            if (o.list) {
                std::printf("%5u %-8s thread %2u stages", j->index, j->compute ? "compute" : "graphics", thread);
                for (auto& s : j->stages) std::printf(" %s", StageName(s->info.stage));
                std::printf("  cache 0x%llx\n", (unsigned long long)j->old_cache);
                break;
            }
            if (o.dump_index >= 0) {
                if (int64_t(j->index) == o.dump_index) {
                    for (auto& s : j->stages) {
                        const std::string path = o.dump_dir + "/" + StageName(s->info.stage) + ".spv";
                        const auto& code = rp.shader_code.at(s->old_module);
                        std::ofstream(path, std::ios::binary)
                            .write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size()));
                        std::printf("wrote %s (%zu bytes)\n", path.c_str(), code.size());
                    }
                    return 0;
                }
                break;
            }
            if (!Selected(o, j->index)) break;
            Wire(rp, *j);
            if (o.threads > 1) deferred.push_back(std::move(j));
            else Run(rp, *j);
            break;
        }
        default:
            std::printf("warning: unknown record tag %u\n", tag);
        }
    }
    // Recorded results of pipelines that faulted in the application have no RESULT record.
    if (o.list) {
        std::printf("pipelines with a recorded result: %zu\n", rp.recorded_result.size());
        return 0;
    }
    if (o.dump_index >= 0) Die("pipeline index not in capture");
    if (!deferred.empty()) {
        std::printf("creating %zu pipelines from %d threads\n", deferred.size(), o.threads);
        std::vector<std::thread> pool;
        for (int t = 0; t < o.threads; ++t)
            pool.emplace_back([&, t] {
                for (size_t k = size_t(t); k < deferred.size(); k += size_t(o.threads)) Run(rp, *deferred[k]);
            });
        for (auto& t : pool) t.join();
    }
    std::printf(rp.failures ? "RESULT: %d pipeline calls failed\n" : "RESULT OK\n", rp.failures.load());
    // Objects are left to process teardown on purpose: a replay must not hang in vkDestroyDevice
    // with live children, and the driver state at exit is not under test.
    return rp.failures ? 3 : 0;
}
