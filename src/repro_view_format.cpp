// Issue #5: render-pass LOAD and blending read an attachment through the *image's* format, not
// the attachment view's format, while the store honours the view.
//
// Setup: a 16x16 R8G8B8A8 image created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, filled with the
// raw byte 128 by vkCmdCopyBufferToImage (format-agnostic). A render pass then uses a view in
// the sibling format as its only colour attachment with loadOp = LOAD, storeOp = STORE, and
// (unless "nodraw") draws a full-screen triangle with blending src = ZERO, dst = ONE, i.e. the
// destination must come out unchanged. The image is copied back and the first texel printed.
//
// Usage: repro_view_format <case> [nodraw]
//   srgb-image-unorm-view   image SRGB, view UNORM   expected 128 128 128 128
//   unorm-image-srgb-view   image UNORM, view SRGB   expected 128 128 128 128
//   control                 image UNORM, view UNORM  expected 128 128 128 128
// If the driver decodes the load through the image format but stores through the view format,
// the first case gives 55 (sRGB 128 -> linear 0.216 -> UNORM 55) and the second 188.
#include "common.h"

static const uint32_t kVert[] = {
#include "fullscreen.vert.inc"
};
static const uint32_t kSolid[] = {
#include "solid.frag.inc"
};

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
};

static Buffer HostBuffer(const Ctx& c, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    CHECK(vkCreateBuffer(c.device, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.device, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(c, req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(c.device, &ai, nullptr, &b.memory));
    CHECK(vkBindBufferMemory(c.device, b.buffer, b.memory, 0));
    return b;
}

static void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                    VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "srgb-image-unorm-view";
    const bool draw = !(argc > 2 && std::strcmp(argv[2], "nodraw") == 0);
    VkFormat image_format, view_format;
    if (std::strcmp(which, "srgb-image-unorm-view") == 0) {
        image_format = VK_FORMAT_R8G8B8A8_SRGB;
        view_format = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (std::strcmp(which, "unorm-image-srgb-view") == 0) {
        image_format = VK_FORMAT_R8G8B8A8_UNORM;
        view_format = VK_FORMAT_R8G8B8A8_SRGB;
    } else if (std::strcmp(which, "control") == 0) {
        image_format = VK_FORMAT_R8G8B8A8_UNORM;
        view_format = VK_FORMAT_R8G8B8A8_UNORM;
    } else {
        std::printf("unknown case %s\n", which);
        return 1;
    }
    std::printf("case: %s, %s\n", which, draw ? "load + blend(ZERO, ONE) draw + store" : "load + store, no draw");
    Ctx c = CreateContext();
    const uint32_t W = 16, H = 16;
    const VkDeviceSize bytes = W * H * 4;

    // Image with a mutable format, filled with raw bytes.
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = image_format;
    ici.extent = {W, H, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image{};
    CHECK(vkCreateImage(c.device, &ici, nullptr, &image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.device, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory image_memory{};
    CHECK(vkAllocateMemory(c.device, &ai, nullptr, &image_memory));
    CHECK(vkBindImageMemory(c.device, image, image_memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = view_format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    CHECK(vkCreateImageView(c.device, &vci, nullptr, &view));

    Buffer upload = HostBuffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buffer readback = HostBuffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    void* mapped = nullptr;
    CHECK(vkMapMemory(c.device, upload.memory, 0, bytes, 0, &mapped));
    std::memset(mapped, 128, size_t(bytes));
    vkUnmapMemory(c.device, upload.memory);

    // Render pass in the VIEW format: load, (draw), store.
    VkAttachmentDescription att{};
    att.format = view_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VkRenderPass rp{};
    CHECK(vkCreateRenderPass(c.device, &rpci, nullptr, &rp));
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &view;
    fci.width = W;
    fci.height = H;
    fci.layers = 1;
    VkFramebuffer fb{};
    CHECK(vkCreateFramebuffer(c.device, &fci, nullptr, &fb));

    // Pipeline: blend src = ZERO, dst = ONE, so the result must equal the loaded destination.
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(c.device, &plci, nullptr, &layout));
    VkShaderModule vs = Module(c.device, kVert, sizeof(kVert));
    VkShaderModule fs = Module(c.device, kSolid, sizeof(kSolid));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    GfxState st(W, H);
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &blend;
    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &st.vi;
    gpci.pInputAssemblyState = &st.ia;
    gpci.pViewportState = &st.vp;
    gpci.pRasterizationState = &st.rs;
    gpci.pMultisampleState = &st.ms;
    gpci.pColorBlendState = &cbs;
    gpci.layout = layout;
    gpci.renderPass = rp;
    VkPipeline pipe{};
    CHECK(vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe));

    VkCommandBuffer cb = BeginOneShot(c);
    Barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {W, H, 1};
    vkCmdCopyBufferToImage(cb, upload.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea = {{0, 0}, {W, H}};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (draw) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);

    Barrier(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    SubmitAndWait(c, cb);

    CHECK(vkMapMemory(c.device, readback.memory, 0, bytes, 0, &mapped));
    const uint8_t* px = static_cast<const uint8_t*>(mapped);
    std::printf("texel (0,0): %u %u %u %u   texel (8,8): %u %u %u %u\n", px[0], px[1], px[2], px[3],
                px[(8 * W + 8) * 4], px[(8 * W + 8) * 4 + 1], px[(8 * W + 8) * 4 + 2], px[(8 * W + 8) * 4 + 3]);
    std::printf("expected:    128 128 128 128\n");
    const bool ok = px[0] == 128 && px[1] == 128 && px[2] == 128 && px[3] == 128;
    std::printf(ok ? "RESULT OK\n" : "RESULT MISMATCH: the loaded value was converted through the image format\n");
    vkUnmapMemory(c.device, readback.memory);
    DestroyContext(c);
    return ok ? 0 : 3;
}
