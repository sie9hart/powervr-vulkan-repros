// Issue #3: vkCreateGraphicsPipelines faults when the subpass has a colour reference set to
// VK_ATTACHMENT_UNUSED. Valid per spec: an unused reference means writes to that location are
// discarded.
//
// Usage: repro_unused_attachment <variant> [msaa2]
//   control          depth-only subpass, colorAttachmentCount = 0           (expected: OK)
//   unused-nowrite   colorAttachmentCount = 1, [0] = UNUSED, FS writes none   (expected: OK)
//   unused-write     colorAttachmentCount = 1, [0] = UNUSED, FS writes loc 0  (expected: OK)
//   msaa2            optional: 2x MSAA depth attachment and rasterization
#include "common.h"

static const uint32_t kVert[] = {
#include "fullscreen.vert.inc"
};
static const uint32_t kNoOutput[] = {
#include "no_output.frag.inc"
};
static const uint32_t kSolid[] = {
#include "solid.frag.inc"
};

int main(int argc, char** argv) {
    const char* variant = argc > 1 ? argv[1] : "unused-nowrite";
    const VkSampleCountFlagBits samples =
        (argc > 2 && std::strcmp(argv[2], "msaa2") == 0) ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
    const bool control = std::strcmp(variant, "control") == 0;
    const bool write = std::strcmp(variant, "unused-write") == 0;
    if (!control && !write && std::strcmp(variant, "unused-nowrite") != 0) {
        std::printf("unknown variant %s\n", variant);
        return 1;
    }
    std::printf("variant: %s, samples: %d\n", variant, int(samples));
    Ctx c = CreateContext();

    // Render pass: one depth attachment, plus (unless control) one UNUSED colour reference.
    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D16_UNORM;
    depth.samples = samples;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference unused_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = control ? 0 : 1;
    sub.pColorAttachments = control ? nullptr : &unused_ref;
    sub.pDepthStencilAttachment = &depth_ref;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &depth;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VkRenderPass rp{};
    CHECK(vkCreateRenderPass(c.device, &rpci, nullptr, &rp));
    std::printf("render pass created\n");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(c.device, &plci, nullptr, &layout));

    VkShaderModule vs = Module(c.device, kVert, sizeof(kVert));
    VkShaderModule fs = write ? Module(c.device, kSolid, sizeof(kSolid))
                              : Module(c.device, kNoOutput, sizeof(kNoOutput));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

    GfxState st(64, 64);
    st.ms.rasterizationSamples = samples;
    st.ds.depthTestEnable = VK_TRUE;
    st.ds.depthWriteEnable = VK_TRUE;
    st.ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // The blend attachment count must equal the subpass colorAttachmentCount.
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = control ? 0 : 1;
    cb.pAttachments = control ? nullptr : &blend;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &st.vi;
    gpci.pInputAssemblyState = &st.ia;
    gpci.pViewportState = &st.vp;
    gpci.pRasterizationState = &st.rs;
    gpci.pMultisampleState = &st.ms;
    gpci.pDepthStencilState = &st.ds;
    gpci.pColorBlendState = &cb;
    gpci.layout = layout;
    gpci.renderPass = rp;

    std::printf("calling vkCreateGraphicsPipelines...\n");
    std::fflush(stdout);
    VkPipeline pipe{};
    CHECK(vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe));
    std::printf("RESULT OK: pipeline created\n");

    vkDestroyPipeline(c.device, pipe, nullptr);
    vkDestroyShaderModule(c.device, vs, nullptr);
    vkDestroyShaderModule(c.device, fs, nullptr);
    vkDestroyPipelineLayout(c.device, layout, nullptr);
    vkDestroyRenderPass(c.device, rp, nullptr);
    DestroyContext(c);
    return 0;
}
