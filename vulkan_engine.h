#pragma once
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <vector>

#include "audio_processor.h"

// Debe coincidir byte a byte con el bloque push_constant de los shaders (56 bytes).
struct PushConstants {
    float resolution[2];
    float phase;
    float amp;
    float bass;
    float treble;
    float speed;
    float glow;
    float cellSize;
    float pxRange;
    float atlasGrid[2];
    float glyphCount;
    float mid;
};
static_assert(sizeof(PushConstants) == 56, "PushConstants debe medir 56 bytes");

class VulkanEngine {
public:
    bool init(ANativeWindow* window, AAssetManager* assets);
    void shutdown();
    // Devuelve false si ocurre un error irrecuperable.
    bool render(const AudioFeatures& audio, float dt);
    void requestResize() { mNeedRecreate = true; }
    bool isReady() const { return mReady; }

private:
    static constexpr uint32_t kFramesInFlight = 2;

    bool createInstance();
    bool createSurface();
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    bool createRenderPasses();
    bool createDescriptors();
    bool createPipelines();
    bool createTargets();
    bool createCommandObjects();
    bool loadAtlas(const char* assetPath);
    bool recreateSwapchain();
    void destroyTargets();
    void destroySwapchain(bool keepHandle);

    VkShaderModule loadShader(const char* assetPath);
    bool makePipeline(const char* fragPath, VkRenderPass rp, VkPipeline* out);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer* buf, VkDeviceMemory* mem);
    bool createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImage* img,
                     VkDeviceMemory* mem);
    bool createView(VkImage img, VkFormat fmt, VkImageView* view);
    void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex, const PushConstants& pc);

    ANativeWindow* mWindow = nullptr;
    AAssetManager* mAssets = nullptr;

    VkInstance mInstance = VK_NULL_HANDLE;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkPhysicalDevice mPhys = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    uint32_t mQueueFamily = 0;
    VkQueue mQueue = VK_NULL_HANDLE;

    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    VkFormat mSwapFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR mSwapColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D mExtent{};
    std::vector<VkImage> mSwapImages;
    std::vector<VkImageView> mSwapViews;
    std::vector<VkFramebuffer> mSwapFbs;
    std::vector<VkSemaphore> mRenderDone;

    // Destino offscreen de la pasada de lluvia (entrada del bloom)
    VkImage mSceneImage = VK_NULL_HANDLE;
    VkDeviceMemory mSceneMem = VK_NULL_HANDLE;
    VkImageView mSceneView = VK_NULL_HANDLE;
    VkFramebuffer mSceneFb = VK_NULL_HANDLE;

    VkRenderPass mSceneRP = VK_NULL_HANDLE;
    VkRenderPass mPostRP = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mPipeLayout = VK_NULL_HANDLE;
    VkPipeline mScenePipe = VK_NULL_HANDLE;
    VkPipeline mPostPipe = VK_NULL_HANDLE;
    VkDescriptorPool mPool = VK_NULL_HANDLE;
    VkDescriptorSet mAtlasSet = VK_NULL_HANDLE;
    VkDescriptorSet mSceneSet = VK_NULL_HANDLE;
    VkSampler mSampler = VK_NULL_HANDLE;

    VkImage mAtlasImage = VK_NULL_HANDLE;
    VkDeviceMemory mAtlasMem = VK_NULL_HANDLE;
    VkImageView mAtlasView = VK_NULL_HANDLE;
    uint32_t mAtlasW = 0, mAtlasH = 0;

    VkCommandPool mCmdPool = VK_NULL_HANDLE;
    VkCommandBuffer mCmd[kFramesInFlight] = {};
    VkFence mFence[kFramesInFlight] = {};
    VkSemaphore mImageAvail[kFramesInFlight] = {};
    uint32_t mFrame = 0;

    bool mReady = false;
    bool mNeedRecreate = false;

    // Estado de animación
    float mPhase = 0.f, mTime = 0.f;
    float mAmp = 0.f, mBass = 0.f, mMid = 0.f, mTreble = 0.f;
};
