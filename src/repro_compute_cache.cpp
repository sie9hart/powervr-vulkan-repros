// Issue #8: vkCreateComputePipelines faults when the VkPipelineCache was created with
// pInitialData from vkGetPipelineCacheData. Graphics pipelines restore from the same kind of
// data correctly.
//
// Usage: repro_compute_cache save <file>   build the pipelines on an empty cache, save its data
//        repro_compute_cache load <file>   seed a cache with that data, build the same pipelines
//        repro_compute_cache inprocess     both steps in one process (second cache seeded)
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
    } else if (std::strcmp(mode, "load") == 0 && argc > 2) {
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::printf("loaded %zu bytes from %s\n", data.size(), argv[2]);
        VkPipelineCache cache = MakeCache(c, &data);
        BuildAll(c, p, cache, "seeded cache");
        std::printf("RESULT OK\n");
    } else if (std::strcmp(mode, "inprocess") == 0) {
        VkPipelineCache first = MakeCache(c, nullptr);
        BuildAll(c, p, first, "empty cache");
        std::vector<uint8_t> data = CacheData(c, first);
        std::printf("cache data: %zu bytes\n", data.size());
        VkPipelineCache second = MakeCache(c, &data);
        BuildAll(c, p, second, "seeded cache");
        std::printf("RESULT OK\n");
    } else {
        std::printf("usage: %s save <file> | load <file> | inprocess\n", argv[0]);
        return 1;
    }
    return 0;
}
