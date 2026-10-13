#include "vulkan_engine.h"

#include <android/imagedecoder.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"

#define VK_CHECK(call)                                                                 \
    do {                                                                               \
        VkResult _r = (call);                                                          \
        if (_r != VK_SUCCESS) {                                                        \
            LOGE("%s fallo: %d (%s:%d)", #call, static_cast<int>(_r), __FILE__, __LINE__); \
            return false;                                                              \
        }                                                                              \
    } while (0)

namespace {
constexpr float kGlyphCount = 58.f;  // matrixcode_msdf.png: 8x8 celdas de 64 px, 57 glifos (celda 4 vacía)
constexpr float kPxRange = 4.f;      // rango de distancia con el que se generó el MSDF

inline float expSmooth(float cur, float target, float up, float down, float dt) {
    const float rate = target > cur ? up : down;
    return cur + (target - cur) * (1.f - std::exp(-rate * dt));
}
}  // namespace

// ---------------------------------------------------------------- init / shutdown

bool VulkanEngine::init(ANativeWindow* window, AAssetManager* assets) {
    if (mReady) return true;
    mWindow = window;
    mAssets = assets;

    if (!createInstance() || !createSurface() || !pickPhysicalDevice() || !createDevice() ||
        !createSwapchain() || !createRenderPasses() || !createDescriptors() ||
        !createPipelines() || !createTargets() || !createCommandObjects() ||
        !loadAtlas("atlas/matrixcode_msdf.png")) {
        LOGE("Inicializacion de Vulkan fallida");
        shutdown();
        return false;
    }
    mReady = true;
    LOGI("Vulkan listo: %ux%u", mExtent.width, mExtent.height);
    return true;
}

void VulkanEngine::shutdown() {
    mReady = false;
    if (mDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(mDevice);

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (mFence[i]) vkDestroyFence(mDevice, mFence[i], nullptr);
            if (mImageAvail[i]) vkDestroySemaphore(mDevice, mImageAvail[i], nullptr);
            mFence[i] = VK_NULL_HANDLE;
            mImageAvail[i] = VK_NULL_HANDLE;
        }
        if (mCmdPool) vkDestroyCommandPool(mDevice, mCmdPool, nullptr);
        mCmdPool = VK_NULL_HANDLE;

        destroySwapchain(false);

        if (mAtlasView) vkDestroyImageView(mDevice, mAtlasView, nullptr);
        if (mAtlasImage) vkDestroyImage(mDevice, mAtlasImage, nullptr);
        if (mAtlasMem) vkFreeMemory(mDevice, mAtlasMem, nullptr);
        mAtlasView = VK_NULL_HANDLE;
        mAtlasImage = VK_NULL_HANDLE;
        mAtlasMem = VK_NULL_HANDLE;

        if (mScenePipe) vkDestroyPipeline(mDevice, mScenePipe, nullptr);
        if (mPostPipe) vkDestroyPipeline(mDevice, mPostPipe, nullptr);
        if (mPipeLayout) vkDestroyPipelineLayout(mDevice, mPipeLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(mDevice, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(mDevice, mSetLayout, nullptr);
        if (mSampler) vkDestroySampler(mDevice, mSampler, nullptr);
        if (mSceneRP) vkDestroyRenderPass(mDevice, mSceneRP, nullptr);
        if (mPostRP) vkDestroyRenderPass(mDevice, mPostRP, nullptr);
        mScenePipe = mPostPipe = VK_NULL_HANDLE;
        mPipeLayout = VK_NULL_HANDLE;
        mPool = VK_NULL_HANDLE;
        mSetLayout = VK_NULL_HANDLE;
        mSampler = VK_NULL_HANDLE;
        mSceneRP = mPostRP = VK_NULL_HANDLE;

        vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
    }
    if (mSurface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
    }
    if (mInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
    mWindow = nullptr;
    mNeedRecreate = false;
}

// ---------------------------------------------------------------- instancia / dispositivo

bool VulkanEngine::createInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MatrixRain";
    app.applicationVersion = 1;
    app.pEngineName = "MatrixRainEngine";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_1;

    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &mInstance));
    return true;
}

bool VulkanEngine::createSurface() {
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = mWindow;
    VK_CHECK(vkCreateAndroidSurfaceKHR(mInstance, &ci, nullptr, &mSurface));
    return true;
}

bool VulkanEngine::pickPhysicalDevice() {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(mInstance, &count, nullptr));
    if (count == 0) {
        LOGE("No hay dispositivos Vulkan");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    VK_CHECK(vkEnumeratePhysicalDevices(mInstance, &count, devs.data()));

    for (auto d : devs) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_1) continue;

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, mSurface, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                mPhys = d;
                mQueueFamily = i;
                LOGI("GPU: %s (Vulkan %u.%u)", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                     VK_VERSION_MINOR(props.apiVersion));
                return true;
            }
        }
    }
    LOGE("Ninguna GPU con Vulkan 1.1 + presentacion");
    return false;
}

bool VulkanEngine::createDevice() {
    const float prio = 1.f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = mQueueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;

    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(mPhys, &ci, nullptr, &mDevice));
    vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);
    return true;
}

// ---------------------------------------------------------------- swapchain

bool VulkanEngine::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhys, mSurface, &caps));

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(mPhys, mSurface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(mPhys, mSurface, &fn, formats.data());
    if (fn == 0) return false;

    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;  // UNORM: los shaders ya escriben en espacio de pantalla
            break;
        }
    }
    // En una recreación el render pass ya depende de este formato: lo conservamos si existe.
    if (mSwapFormat != VK_FORMAT_UNDEFINED) {
        for (auto& f : formats)
            if (f.format == mSwapFormat) chosen = f;
    }
    mSwapFormat = chosen.format;
    mSwapColorSpace = chosen.colorSpace;

    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        mExtent = caps.currentExtent;
    } else {
        mExtent.width = static_cast<uint32_t>(ANativeWindow_getWidth(mWindow));
        mExtent.height = static_cast<uint32_t>(ANativeWindow_getHeight(mWindow));
    }
    if (mExtent.width == 0 || mExtent.height == 0) return false;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR prefs[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    for (auto a : prefs) {
        if (caps.supportedCompositeAlpha & a) {
            alpha = a;
            break;
        }
    }

    VkSwapchainKHR old = mSwapchain;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = mSurface;
    ci.minImageCount = imageCount;
    ci.imageFormat = mSwapFormat;
    ci.imageColorSpace = mSwapColorSpace;
    ci.imageExtent = mExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;  // la app está bloqueada en vertical
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(mDevice, &ci, nullptr, &mSwapchain));
    if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR(mDevice, old, nullptr);

    uint32_t n = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(mDevice, mSwapchain, &n, nullptr));
    mSwapImages.resize(n);
    VK_CHECK(vkGetSwapchainImagesKHR(mDevice, mSwapchain, &n, mSwapImages.data()));

    mSwapViews.assign(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i)
        if (!createView(mSwapImages[i], mSwapFormat, &mSwapViews[i])) return false;

    mRenderDone.assign(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) VK_CHECK(vkCreateSemaphore(mDevice, &sci, nullptr, &mRenderDone[i]));
    return true;
}

void VulkanEngine::destroyTargets() {
    for (auto fb : mSwapFbs) vkDestroyFramebuffer(mDevice, fb, nullptr);
    mSwapFbs.clear();
    if (mSceneFb) vkDestroyFramebuffer(mDevice, mSceneFb, nullptr);
    if (mSceneView) vkDestroyImageView(mDevice, mSceneView, nullptr);
    if (mSceneImage) vkDestroyImage(mDevice, mSceneImage, nullptr);
    if (mSceneMem) vkFreeMemory(mDevice, mSceneMem, nullptr);
    mSceneFb = VK_NULL_HANDLE;
    mSceneView = VK_NULL_HANDLE;
    mSceneImage = VK_NULL_HANDLE;
    mSceneMem = VK_NULL_HANDLE;
}

// keepHandle=true conserva mSwapchain para usarlo como oldSwapchain en la recreación.
void VulkanEngine::destroySwapchain(bool keepHandle) {
    destroyTargets();
    for (auto s : mRenderDone) vkDestroySemaphore(mDevice, s, nullptr);
    mRenderDone.clear();
    for (auto v : mSwapViews) vkDestroyImageView(mDevice, v, nullptr);
    mSwapViews.clear();
    mSwapImages.clear();
    if (!keepHandle && mSwapchain) {
        vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
}

bool VulkanEngine::recreateSwapchain() {
    vkDeviceWaitIdle(mDevice);
    destroySwapchain(true);
    if (!createSwapchain()) {
        LOGW("recreateSwapchain: superficie no disponible todavia");
        return false;
    }
    if (!createTargets()) return false;
    mNeedRecreate = false;
    LOGI("Swapchain recreada: %ux%u", mExtent.width, mExtent.height);
    return true;
}

// ---------------------------------------------------------------- render passes

bool VulkanEngine::createRenderPasses() {
    // --- Pasada 1: lluvia -> imagen offscreen RGBA8, que luego se muestrea
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // el shader cubre toda la pantalla
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;  // el bloom del frame anterior aun lee
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = 0;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(mDevice, &ci, nullptr, &mSceneRP));
    }
    // --- Pasada 2: bloom + composición -> swapchain
    {
        VkAttachmentDescription att{};
        att.format = mSwapFormat;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;  // espera el semaforo de adquisicion
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 1;
        ci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(mDevice, &ci, nullptr, &mPostRP));
    }
    return true;
}

// ---------------------------------------------------------------- descriptores / pipelines

bool VulkanEngine::createDescriptors() {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.f;
    VK_CHECK(vkCreateSampler(mDevice, &sci, nullptr, &mSampler));

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(mDevice, &lci, nullptr, &mSetLayout));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(mDevice, &pci, nullptr, &mPool));

    VkDescriptorSetLayout layouts[2] = {mSetLayout, mSetLayout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = mPool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(mDevice, &ai, sets));
    mAtlasSet = sets[0];
    mSceneSet = sets[1];

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &mSetLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(mDevice, &plci, nullptr, &mPipeLayout));
    return true;
}

VkShaderModule VulkanEngine::loadShader(const char* path) {
    AAsset* a = AAssetManager_open(mAssets, path, AASSET_MODE_BUFFER);
    if (!a) {
        LOGE("Shader no encontrado en assets: %s", path);
        return VK_NULL_HANDLE;
    }
    const size_t len = static_cast<size_t>(AAsset_getLength(a));
    std::vector<uint32_t> code((len + 3) / 4);
    AAsset_read(a, code.data(), len);
    AAsset_close(a);

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = len;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(mDevice, &ci, nullptr, &m) != VK_SUCCESS) {
        LOGE("vkCreateShaderModule fallo: %s", path);
        return VK_NULL_HANDLE;
    }
    return m;
}

bool VulkanEngine::makePipeline(const char* fragPath, VkRenderPass rp, VkPipeline* out) {
    VkShaderModule vs = loadShader("shaders/matrix.vert.spv");
    VkShaderModule fs = loadShader(fragPath);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(mDevice, vs, nullptr);
        if (fs) vkDestroyShaderModule(mDevice, fs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = mPipeLayout;
    ci.renderPass = rp;
    ci.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &ci, nullptr, out);
    vkDestroyShaderModule(mDevice, vs, nullptr);
    vkDestroyShaderModule(mDevice, fs, nullptr);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines fallo (%s): %d", fragPath, static_cast<int>(r));
        return false;
    }
    return true;
}

bool VulkanEngine::createPipelines() {
    return makePipeline("shaders/matrix.frag.spv", mSceneRP, &mScenePipe) &&
           makePipeline("shaders/bloom.frag.spv", mPostRP, &mPostPipe);
}

// ---------------------------------------------------------------- recursos de imagen

uint32_t VulkanEngine::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(mPhys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

bool VulkanEngine::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags props, VkBuffer* buf, VkDeviceMemory* mem) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(mDevice, &bi, nullptr, buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mDevice, *buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    VK_CHECK(vkAllocateMemory(mDevice, &ai, nullptr, mem));
    VK_CHECK(vkBindBufferMemory(mDevice, *buf, *mem, 0));
    return true;
}

bool VulkanEngine::createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                               VkImage* img, VkDeviceMemory* mem) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(mDevice, &ci, nullptr, img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(mDevice, *img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    VK_CHECK(vkAllocateMemory(mDevice, &ai, nullptr, mem));
    VK_CHECK(vkBindImageMemory(mDevice, *img, *mem, 0));
    return true;
}

bool VulkanEngine::createView(VkImage img, VkFormat fmt, VkImageView* view) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = img;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(mDevice, &ci, nullptr, view));
    return true;
}

// Imagen offscreen + framebuffers; actualiza el descriptor que lee el bloom.
bool VulkanEngine::createTargets() {
    if (!createImage(mExtent.width, mExtent.height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &mSceneImage,
                     &mSceneMem) ||
        !createView(mSceneImage, VK_FORMAT_R8G8B8A8_UNORM, &mSceneView))
        return false;

    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = mSceneRP;
    fci.attachmentCount = 1;
    fci.pAttachments = &mSceneView;
    fci.width = mExtent.width;
    fci.height = mExtent.height;
    fci.layers = 1;
    VK_CHECK(vkCreateFramebuffer(mDevice, &fci, nullptr, &mSceneFb));

    mSwapFbs.assign(mSwapViews.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < mSwapViews.size(); ++i) {
        fci.renderPass = mPostRP;
        fci.pAttachments = &mSwapViews[i];
        VK_CHECK(vkCreateFramebuffer(mDevice, &fci, nullptr, &mSwapFbs[i]));
    }

    VkDescriptorImageInfo ii{mSampler, mSceneView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = mSceneSet;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(mDevice, 1, &w, 0, nullptr);
    return true;
}

bool VulkanEngine::createCommandObjects() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = mQueueFamily;
    VK_CHECK(vkCreateCommandPool(mDevice, &pci, nullptr, &mCmdPool));

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mCmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(mDevice, &ai, mCmd));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VK_CHECK(vkCreateFence(mDevice, &fci, nullptr, &mFence[i]));
        VK_CHECK(vkCreateSemaphore(mDevice, &sci, nullptr, &mImageAvail[i]));
    }
    return true;
}

// ---------------------------------------------------------------- atlas MSDF (PNG -> VkImage)

bool VulkanEngine::loadAtlas(const char* path) {
    AAsset* asset = AAssetManager_open(mAssets, path, AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("Atlas no encontrado: %s", path);
        return false;
    }
    const void* data = AAsset_getBuffer(asset);
    const size_t dataLen = static_cast<size_t>(AAsset_getLength(asset));

    AImageDecoder* dec = nullptr;
    if (AImageDecoder_createFromBuffer(data, dataLen, &dec) != ANDROID_IMAGE_DECODER_SUCCESS) {
        LOGE("AImageDecoder no pudo abrir %s", path);
        AAsset_close(asset);
        return false;
    }
    AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888);
    const AImageDecoderHeaderInfo* hdr = AImageDecoder_getHeaderInfo(dec);
    mAtlasW = static_cast<uint32_t>(AImageDecoderHeaderInfo_getWidth(hdr));
    mAtlasH = static_cast<uint32_t>(AImageDecoderHeaderInfo_getHeight(hdr));
    const size_t stride = AImageDecoder_getMinimumStride(dec);
    std::vector<uint8_t> pixels(stride * mAtlasH);
    const int rc = AImageDecoder_decodeImage(dec, pixels.data(), stride, pixels.size());
    AImageDecoder_delete(dec);
    AAsset_close(asset);
    if (rc != ANDROID_IMAGE_DECODER_SUCCESS) {
        LOGE("Decodificacion PNG fallo: %d", rc);
        return false;
    }
    LOGI("Atlas MSDF %s: %ux%u", path, mAtlasW, mAtlasH);

    // Buffer de staging
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!createBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &staging, &stagingMem))
        return false;
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(mDevice, stagingMem, 0, VK_WHOLE_SIZE, 0, &mapped));
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(mDevice, stagingMem);

    // UNORM (no sRGB): el MSDF son datos de distancia, no color
    if (!createImage(mAtlasW, mAtlasH, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &mAtlasImage,
                     &mAtlasMem) ||
        !createView(mAtlasImage, VK_FORMAT_R8G8B8A8_UNORM, &mAtlasView))
        return false;

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mCmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(mDevice, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = mAtlasImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.bufferRowLength = static_cast<uint32_t>(stride / 4);
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {mAtlasW, mAtlasH, 1};
    vkCmdCopyBufferToImage(cmd, staging, mAtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(mQueue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(mQueue));

    vkFreeCommandBuffers(mDevice, mCmdPool, 1, &cmd);
    vkDestroyBuffer(mDevice, staging, nullptr);
    vkFreeMemory(mDevice, stagingMem, nullptr);

    VkDescriptorImageInfo ii{mSampler, mAtlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = mAtlasSet;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(mDevice, 1, &w, 0, nullptr);
    return true;
}

// ---------------------------------------------------------------- frame

void VulkanEngine::recordFrame(VkCommandBuffer cmd, uint32_t imageIndex, const PushConstants& pc) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkViewport vp{0.f, 0.f, static_cast<float>(mExtent.width), static_cast<float>(mExtent.height), 0.f, 1.f};
    VkRect2D sc{{0, 0}, mExtent};

    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderArea = sc;

    // Pasada 1: lluvia con decodificación MSDF -> imagen offscreen
    rb.renderPass = mSceneRP;
    rb.framebuffer = mSceneFb;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mScenePipe);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeLayout, 0, 1, &mAtlasSet, 0,
                            nullptr);
    vkCmdPushConstants(cmd, mPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // Pasada 2: bloom + viñeta -> swapchain
    rb.renderPass = mPostRP;
    rb.framebuffer = mSwapFbs[imageIndex];
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPostPipe);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeLayout, 0, 1, &mSceneSet, 0,
                            nullptr);
    vkCmdPushConstants(cmd, mPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    vkEndCommandBuffer(cmd);
}

bool VulkanEngine::render(const AudioFeatures& audio, float dt) {
    if (!mReady) return false;
    dt = std::clamp(dt, 0.f, 0.1f);
    mTime += dt;

    if (mNeedRecreate) {
        if (!recreateSwapchain()) return true;  // minimizada / sin superficie: reintenta luego
    }

    // --- Modulación por audio (o animación de reposo si no hay captura)
    float a = audio.amplitude, b = audio.bass, m = audio.mid, t = audio.treble;
    if (!audio.live) {
        a = 0.25f + 0.12f * std::sin(mTime * 0.7f);
        b = 0.20f + 0.15f * std::sin(mTime * 1.3f);
        m = 0.20f;
        t = 0.15f + 0.1f * std::sin(mTime * 0.5f);
    }
    mAmp = expSmooth(mAmp, a, 30.f, 5.f, dt);
    mBass = expSmooth(mBass, b, 35.f, 6.f, dt);
    mMid = expSmooth(mMid, m, 25.f, 5.f, dt);
    mTreble = expSmooth(mTreble, t, 25.f, 5.f, dt);

    const float speed = 0.7f + 2.8f * mBass + 1.6f * mAmp;  // la velocidad de la lluvia sigue al audio
    mPhase = std::fmod(mPhase + dt * speed, 65536.f);
    const float glow = 0.55f + 1.4f * mAmp + 0.6f * mBass;   // el brillo del bloom sigue al audio

    PushConstants pc{};
    pc.resolution[0] = static_cast<float>(mExtent.width);
    pc.resolution[1] = static_cast<float>(mExtent.height);
    pc.phase = mPhase;
    pc.amp = mAmp;
    pc.bass = mBass;
    pc.treble = mTreble;
    pc.mid = mMid;
    pc.speed = speed;
    pc.glow = glow;
    pc.cellSize = std::max(16.f, static_cast<float>(mExtent.width) / 40.f);
    pc.pxRange = kPxRange;
    pc.atlasGrid[0] = std::max(1.f, static_cast<float>(mAtlasW / 64));
    pc.atlasGrid[1] = std::max(1.f, static_cast<float>(mAtlasH / 64));
    pc.glyphCount = kGlyphCount;

    // --- Ciclo de frame
    const uint32_t f = mFrame;
    vkWaitForFences(mDevice, 1, &mFence[f], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, mImageAvail[f],
                                       VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        mNeedRecreate = true;
        return true;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR: %d", static_cast<int>(r));
        return false;
    }
    vkResetFences(mDevice, 1, &mFence[f]);

    vkResetCommandBuffer(mCmd[f], 0);
    recordFrame(mCmd[f], imageIndex, pc);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &mImageAvail[f];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &mCmd[f];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &mRenderDone[imageIndex];
    if (vkQueueSubmit(mQueue, 1, &si, mFence[f]) != VK_SUCCESS) {
        LOGE("vkQueueSubmit fallo");
        return false;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &mRenderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(mQueue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        mNeedRecreate = true;
    } else if (r != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR: %d", static_cast<int>(r));
        return false;
    }

    mFrame = (mFrame + 1) % kFramesInFlight;
    return true;
}
