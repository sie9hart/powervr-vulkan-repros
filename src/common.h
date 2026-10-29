// Minimal Vulkan 1.1 bootstrap shared by the repro programs. No window, no framework.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(call)                                                                        \
    do {                                                                                   \
        VkResult r_ = (call);                                                              \
        if (r_ != VK_SUCCESS) {                                                            \
            std::printf("FAIL %s -> VkResult %d (%s:%d)\n", #call, r_, __FILE__, __LINE__); \
            std::exit(2);                                                                  \
        }                                                                                  \
    } while (0)

struct Ctx {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queue_family = 0;
    VkCommandPool pool{};
    VkPhysicalDeviceProperties props{};
};

inline Ctx CreateContext() {
    Ctx c;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "powervr-repro";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, nullptr, &c.instance));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(c.instance, &n, nullptr));
    if (n == 0) { std::printf("FAIL no Vulkan device\n"); std::exit(2); }
    std::vector<VkPhysicalDevice> gpus(n);
    CHECK(vkEnumeratePhysicalDevices(c.instance, &n, gpus.data()));
    c.gpu = gpus[0];
    vkGetPhysicalDeviceProperties(c.gpu, &c.props);
    std::printf("device: %s  api %u.%u.%u  driver 0x%08x  vendor 0x%04x  id 0x%08x\n",
                c.props.deviceName, VK_VERSION_MAJOR(c.props.apiVersion),
                VK_VERSION_MINOR(c.props.apiVersion), VK_VERSION_PATCH(c.props.apiVersion),
                c.props.driverVersion, c.props.vendorID, c.props.deviceID);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i) {
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { c.queue_family = i; break; }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = c.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    CHECK(vkCreateDevice(c.gpu, &dci, nullptr, &c.device));
    vkGetDeviceQueue(c.device, c.queue_family, 0, &c.queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c.queue_family;
    CHECK(vkCreateCommandPool(c.device, &pci, nullptr, &c.pool));
    return c;
}

inline void DestroyContext(Ctx& c) {
    vkDeviceWaitIdle(c.device);
    vkDestroyCommandPool(c.device, c.pool, nullptr);
    vkDestroyDevice(c.device, nullptr);
    vkDestroyInstance(c.instance, nullptr);
}

inline VkShaderModule Module(VkDevice d, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m{};
    CHECK(vkCreateShaderModule(d, &ci, nullptr, &m));
    return m;
}

inline uint32_t MemoryType(const Ctx& c, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c.gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    std::printf("FAIL no memory type\n");
    std::exit(2);
}

inline VkCommandBuffer BeginOneShot(const Ctx& c) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = c.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    CHECK(vkAllocateCommandBuffers(c.device, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

inline void SubmitAndWait(const Ctx& c, VkCommandBuffer cb) {
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(c.queue));
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
}

// Fixed-function state shared by the graphics repros.
struct GfxState {
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkViewport viewport{};
    VkRect2D scissor{};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    GfxState(uint32_t w, uint32_t h) {
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        viewport = {0, 0, float(w), float(h), 0, 1};
        scissor = {{0, 0}, {w, h}};
        vp.viewportCount = 1;
        vp.pViewports = &viewport;
        vp.scissorCount = 1;
        vp.pScissors = &scissor;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    }
};
