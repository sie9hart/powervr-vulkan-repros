// Issue #8: vkCreateComputePipelines faults when the VkPipelineCache was created with
// pInitialData from vkGetPipelineCacheData. Graphics pipelines restore from the same kind of
// data correctly.
//
// Usage: repro_compute_cache save <file>   build the pipelines on an empty cache, save its data
//        repro_compute_cache load <file>   seed a cache with that data, build the same pipelines
//        repro_compute_cache inprocess     both steps in one process (second cache seeded)
//        repro_compute_cache mixed         like inprocess, but the cache also holds 40 graphics
//                                          pipelines built before the compute ones (the
//                                          application that hit the fault used one cache for both)
// Expected: every step prints RESULT OK.
#include "common.h"
#include <fstream>
#include <iterator>

static const uint32_t kBufferComp[] = {
#include "buffer.comp.inc"
};
static const uint32_t kImageComp[] = {
#include "image.comp.inc"
};
static const uint32_t kVert[] = {
#include "fullscreen.vert.inc"
};
static const uint32_t kHeavy[] = {
#include "heavy.frag.inc"
};

// 40 distinct graphics pipelines (one specialization constant each) into `cache`.
static void BuildGraphics(const Ctx& c, VkPipelineCache cache, const char* tag) {
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VkRenderPass rp{};
    CHECK(vkCreateRenderPass(c.device, &rpci, nullptr, &rp));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(c.device, &plci, nullptr, &layout));
    VkShaderModule vs = Module(c.device, kVert, sizeof(kVert));
    VkShaderModule fs = Module(c.device, kHeavy, sizeof(kHeavy));
    GfxState st(64, 64);
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    for (int32_t seed = 0; seed < 40; ++seed) {
        VkSpecializationMapEntry entry{0, 0, sizeof(int32_t)};
        VkSpecializationInfo spec{1, &entry, sizeof(seed), &seed};
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", &spec};
        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &st.vi;
        gpci.pInputAssemblyState = &st.ia;
        gpci.pViewportState = &st.vp;
        gpci.pRasterizationState = &st.rs;
        gpci.pMultisampleState = &st.ms;
        gpci.pColorBlendState = &cb;
        gpci.layout = layout;
        gpci.renderPass = rp;
        VkPipeline pipe{};
        CHECK(vkCreateGraphicsPipelines(c.device, cache, 1, &gpci, nullptr, &pipe));
        vkDestroyPipeline(c.device, pipe, nullptr);
    }
    std::printf("[%s] 40 graphics pipelines OK\n", tag);
    vkDestroyShaderModule(c.device, vs, nullptr);
    vkDestroyShaderModule(c.device, fs, nullptr);
    vkDestroyPipelineLayout(c.device, layout, nullptr);
    vkDestroyRenderPass(c.device, rp, nullptr);
}

struct Pipelines {
    VkDescriptorSetLayout set_layouts[2]{};
    VkPipelineLayout layouts[2]{};
    VkShaderModule modules[2]{};
};

static Pipelines MakeLayouts(const Ctx& c) {
    Pipelines p;
    // Layout 0: one storage buffer + push constants.
    VkDescriptorSetLayoutBinding b0{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo d0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    d0.bindingCount = 1;
    d0.pBindings = &b0;
    CHECK(vkCreateDescriptorSetLayout(c.device, &d0, nullptr, &p.set_layouts[0]));
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo l0{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    l0.setLayoutCount = 1;
    l0.pSetLayouts = &p.set_layouts[0];
    l0.pushConstantRangeCount = 1;
    l0.pPushConstantRanges = &pc;
    CHECK(vkCreatePipelineLayout(c.device, &l0, nullptr, &p.layouts[0]));
    // Layout 1: storage image + storage buffer.
    VkDescriptorSetLayoutBinding b1[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo d1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    d1.bindingCount = 2;
    d1.pBindings = b1;
    CHECK(vkCreateDescriptorSetLayout(c.device, &d1, nullptr, &p.set_layouts[1]));
    VkPipelineLayoutCreateInfo l1{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    l1.setLayoutCount = 1;
    l1.pSetLayouts = &p.set_layouts[1];
    CHECK(vkCreatePipelineLayout(c.device, &l1, nullptr, &p.layouts[1]));
    p.modules[0] = Module(c.device, kBufferComp, sizeof(kBufferComp));
    p.modules[1] = Module(c.device, kImageComp, sizeof(kImageComp));
    return p;
}

static void BuildAll(const Ctx& c, const Pipelines& p, VkPipelineCache cache, const char* tag) {
    for (int i = 0; i < 2; ++i) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, p.modules[i], "main", nullptr};
        ci.layout = p.layouts[i];
        std::printf("[%s] vkCreateComputePipelines #%d...\n", tag, i);
        std::fflush(stdout);
        VkPipeline pipe{};
        CHECK(vkCreateComputePipelines(c.device, cache, 1, &ci, nullptr, &pipe));
        std::printf("[%s] compute pipeline #%d OK\n", tag, i);
        vkDestroyPipeline(c.device, pipe, nullptr);
    }
}

static std::vector<uint8_t> CacheData(const Ctx& c, VkPipelineCache cache) {
    size_t size = 0;
    CHECK(vkGetPipelineCacheData(c.device, cache, &size, nullptr));
    std::vector<uint8_t> data(size);
    CHECK(vkGetPipelineCacheData(c.device, cache, &size, data.data()));
    data.resize(size);
    return data;
}

static VkPipelineCache MakeCache(const Ctx& c, const std::vector<uint8_t>* seed) {
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (seed) {
        ci.initialDataSize = seed->size();
        ci.pInitialData = seed->data();
    }
    VkPipelineCache cache{};
    CHECK(vkCreatePipelineCache(c.device, &ci, nullptr, &cache));
    return cache;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "inprocess";
    std::printf("mode: %s\n", mode);
    Ctx c = CreateContext();
    Pipelines p = MakeLayouts(c);

    if (std::strcmp(mode, "save") == 0 && argc > 2) {
        VkPipelineCache cache = MakeCache(c, nullptr);
        BuildAll(c, p, cache, "empty cache");
        std::vector<uint8_t> data = CacheData(c, cache);
        std::ofstream(argv[2], std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        std::printf("saved %zu bytes to %s\nRESULT OK\n", data.size(), argv[2]);
        vkDestroyPipelineCache(c.device, cache, nullptr);
    } else if (std::strcmp(mode, "load") == 0 && argc > 2) {
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::printf("loaded %zu bytes from %s\n", data.size(), argv[2]);
        VkPipelineCache cache = MakeCache(c, &data);
        BuildAll(c, p, cache, "seeded cache");
        std::printf("RESULT OK\n");
        vkDestroyPipelineCache(c.device, cache, nullptr);
    } else if (std::strcmp(mode, "inprocess") == 0) {
        VkPipelineCache first = MakeCache(c, nullptr);
        BuildAll(c, p, first, "empty cache");
        std::vector<uint8_t> data = CacheData(c, first);
        std::printf("cache data: %zu bytes\n", data.size());
        VkPipelineCache second = MakeCache(c, &data);
        BuildAll(c, p, second, "seeded cache");
        std::printf("RESULT OK\n");
        vkDestroyPipelineCache(c.device, first, nullptr);
        vkDestroyPipelineCache(c.device, second, nullptr);
    } else if (std::strcmp(mode, "mixed") == 0) {
        VkPipelineCache first = MakeCache(c, nullptr);
        BuildGraphics(c, first, "empty cache");
        BuildAll(c, p, first, "empty cache");
        std::vector<uint8_t> data = CacheData(c, first);
        std::printf("cache data: %zu bytes\n", data.size());
        VkPipelineCache second = MakeCache(c, &data);
        BuildGraphics(c, second, "seeded cache");
        BuildAll(c, p, second, "seeded cache");
        std::printf("RESULT OK\n");
        vkDestroyPipelineCache(c.device, first, nullptr);
        vkDestroyPipelineCache(c.device, second, nullptr);
    } else {
        std::printf("usage: %s save <file> | load <file> | inprocess | mixed\n", argv[0]);
        return 1;
    }
    for (int i = 0; i < 2; ++i) {
        vkDestroyShaderModule(c.device, p.modules[i], nullptr);
        vkDestroyPipelineLayout(c.device, p.layouts[i], nullptr);
        vkDestroyDescriptorSetLayout(c.device, p.set_layouts[i], nullptr);
    }
    DestroyContext(c);
    return 0;
}
