// Issue #7: the shader compiler (libufwriter.so, BILParseStream) faults when two threads create
// pipelines at the same time, each with its own VkPipelineCache. The spec allows concurrent
// vkCreateShaderModule / vkCreateGraphicsPipelines calls on one VkDevice.
//
// Usage: repro_concurrent_compile [threads] [pipelines_per_thread] [serial]
//   default 4 threads x 60 pipelines. "serial" adds a global mutex (control run).
// Expected: RESULT OK in both modes.
#include "common.h"
#include <atomic>
#include <mutex>
#include <thread>

static const uint32_t kVert[] = {
#include "fullscreen.vert.inc"
};
static const uint32_t kHeavy[] = {
#include "heavy.frag.inc"
};

static std::mutex g_compile_lock;
static bool g_serial = false;
static std::atomic<int> g_done{0};

static void Worker(const Ctx* c, VkRenderPass rp, VkPipelineLayout layout, int thread_index, int count) {
    VkPipelineCacheCreateInfo cci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache{};
    CHECK(vkCreatePipelineCache(c->device, &cci, nullptr, &cache));
    GfxState st(64, 64);
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    for (int i = 0; i < count; ++i) {
        // A unique specialization constant per pipeline, so nothing is served from a cache.
        int32_t seed = thread_index * 100000 + i;
        VkSpecializationMapEntry entry{0, 0, sizeof(int32_t)};
        VkSpecializationInfo spec{1, &entry, sizeof(seed), &seed};

        std::unique_lock<std::mutex> lock(g_compile_lock, std::defer_lock);
        if (g_serial) lock.lock();
        VkShaderModule vs = Module(c->device, kVert, sizeof(kVert));
        VkShaderModule fs = Module(c->device, kHeavy, sizeof(kHeavy));
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
        CHECK(vkCreateGraphicsPipelines(c->device, cache, 1, &gpci, nullptr, &pipe));
        if (g_serial) lock.unlock();

        vkDestroyPipeline(c->device, pipe, nullptr);
        vkDestroyShaderModule(c->device, vs, nullptr);
        vkDestroyShaderModule(c->device, fs, nullptr);
        int done = ++g_done;
        if (done % 20 == 0) {
            std::printf("  %d pipelines created\n", done);
            std::fflush(stdout);
        }
    }
    vkDestroyPipelineCache(c->device, cache, nullptr);
}

int main(int argc, char** argv) {
    int threads = argc > 1 ? std::atoi(argv[1]) : 4;
    int per_thread = argc > 2 ? std::atoi(argv[2]) : 60;
    g_serial = argc > 3 && std::strcmp(argv[3], "serial") == 0;
    if (threads < 1) threads = 1;
    if (per_thread < 1) per_thread = 1;
    std::printf("threads: %d, pipelines per thread: %d, mode: %s\n", threads, per_thread,
                g_serial ? "serial (global mutex)" : "concurrent");
    Ctx c = CreateContext();

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

    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(Worker, &c, rp, layout, t, per_thread);
    for (auto& t : pool) t.join();

    std::printf("RESULT OK: %d pipelines created\n", g_done.load());
    vkDestroyPipelineLayout(c.device, layout, nullptr);
    vkDestroyRenderPass(c.device, rp, nullptr);
    DestroyContext(c);
    return 0;
}
