// Draws the ImGui menu on top of the game's swapchain image right before it is presented.
// One ImGui context, bound to the first device/graphics queue that presents.

#include "overlay.h"

#include "cheats.h"
#include "gamepad.h"
#include "input.h"
#include "log.h"
#include "menu.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace overlay {
namespace {

struct SwapImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
};

struct Swapchain {
    DeviceData* dev = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent = {};
    std::vector<SwapImage> images;
    bool built = false;
};

// ImGui keeps a ring of vertex buffers this long; must cover every image in flight.
constexpr uint32_t kImGuiRingSize = 8;
constexpr double kWatermarkSeconds = 20.0;
constexpr uint64_t kFenceTimeoutNs = 1'000'000'000ull;

std::mutex g_mutex;
std::unordered_map<VkSwapchainKHR, Swapchain> g_swapchains;

DeviceData* g_dev = nullptr;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_queueFamily = 0;
VkRenderPass g_renderPass = VK_NULL_HANDLE;
VkFormat g_renderPassFormat = VK_FORMAT_UNDEFINED;
bool g_backendReady = false;
bool g_contextReady = false;
bool g_warnedQueue = false;
LARGE_INTEGER g_freq = {};
LARGE_INTEGER g_initTime = {};
LARGE_INTEGER g_lastFrame = {};
std::string g_iniPath;

double secondsSince(const LARGE_INTEGER& t)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - t.QuadPart) / double(g_freq.QuadPart);
}

// ImGui colours are sRGB. On an HDR10 swapchain (PQ, BT.2020) they would be read as PQ codes, so
// white lands near 10000 nits. Re-encode every vertex colour: sRGB -> linear BT.709 -> BT.2020 ->
// nits at paper white -> PQ. The font atlas is white, so vertex colours carry all the colour.
constexpr float kPaperWhiteNits = 200.0f;

float pqEncode(float nits)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return powf((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

ImU32 hdr10Colour(ImU32 c)
{
    static float lin[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) {
            float v = i / 255.0f;
            lin[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
        }
        ready = true;
    }
    float r = lin[c & 0xFF], g = lin[(c >> 8) & 0xFF], b = lin[(c >> 16) & 0xFF];
    float r2 = 0.6274f * r + 0.3293f * g + 0.0433f * b;
    float g2 = 0.0691f * r + 0.9195f * g + 0.0114f * b;
    float b2 = 0.0164f * r + 0.0880f * g + 0.8956f * b;
    auto q = [](float v) { return ImU32(pqEncode(v * kPaperWhiteNits) * 255.0f + 0.5f); };
    return q(r2) | (q(g2) << 8) | (q(b2) << 16) | (c & 0xFF000000);
}

void encodeHdr10(ImDrawData* dd)
{
    std::unordered_map<ImU32, ImU32> cache;
    for (ImDrawList* list : dd->CmdLists) {
        for (ImDrawVert& v : list->VtxBuffer) {
            auto it = cache.find(v.col);
            if (it == cache.end())
                it = cache.emplace(v.col, hdr10Colour(v.col)).first;
            v.col = it->second;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL imguiAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info,
                                                           VkCommandBuffer* out)
{
    return layerAllocateCommandBuffers(g_dev, info, out);
}

// ImGui talks to the next layer directly, never back through our own intercepts.
PFN_vkVoidFunction imguiLoader(const char* name, void*)
{
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&imguiAllocateCommandBuffers);
    if (PFN_vkVoidFunction fn = g_dev->GetDeviceProcAddr(g_dev->device, name))
        return fn;
    return g_dev->instance->GetInstanceProcAddr(g_dev->instance->instance, name);
}

void releaseImages(Swapchain& sc)
{
    DeviceData* d = sc.dev;
    for (SwapImage& im : sc.images) {
        if (im.fence)
            d->WaitForFences(d->device, 1, &im.fence, VK_TRUE, kFenceTimeoutNs);
        if (im.framebuffer)
            d->DestroyFramebuffer(d->device, im.framebuffer, nullptr);
        if (im.view)
            d->DestroyImageView(d->device, im.view, nullptr);
        if (im.pool)
            d->DestroyCommandPool(d->device, im.pool, nullptr);
        if (im.fence)
            d->DestroyFence(d->device, im.fence, nullptr);
        if (im.done)
            d->DestroySemaphore(d->device, im.done, nullptr);
        VkImage keep = im.image;
        im = SwapImage{};
        im.image = keep;
    }
    sc.built = false;
}

VkFence createSignaledFence(DeviceData* d)
{
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    d->CreateFence(d->device, &fci, nullptr, &fence);
    return fence;
}

bool buildImages(Swapchain& sc)
{
    DeviceData* d = sc.dev;
    for (SwapImage& im : sc.images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = im.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sc.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (d->CreateImageView(d->device, &vci, nullptr, &im.view) != VK_SUCCESS)
            goto fail;

        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = g_renderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &im.view;
        fbci.width = sc.extent.width;
        fbci.height = sc.extent.height;
        fbci.layers = 1;
        if (d->CreateFramebuffer(d->device, &fbci, nullptr, &im.framebuffer) != VK_SUCCESS)
            goto fail;

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = g_queueFamily;
        if (d->CreateCommandPool(d->device, &pci, nullptr, &im.pool) != VK_SUCCESS)
            goto fail;

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = im.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (layerAllocateCommandBuffers(d, &cai, &im.cmd) != VK_SUCCESS)
            goto fail;

        im.fence = createSignaledFence(d);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!im.fence || d->CreateSemaphore(d->device, &sci, nullptr, &im.done) != VK_SUCCESS)
            goto fail;
    }
    sc.built = true;
    LOG("overlay resources built for %u images (%ux%u)", (unsigned)sc.images.size(), sc.extent.width,
        sc.extent.height);
    return true;

fail:
    LOG("overlay resource creation failed, overlay disabled for this swapchain");
    releaseImages(sc);
    return false;
}

VkRenderPass createRenderPass(DeviceData* d, VkFormat format)
{
    // Load the game's finished frame and draw on top; layout stays PRESENT_SRC on both ends.
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (d->CreateRenderPass(d->device, &rpci, nullptr, &rp) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return rp;
}

void initContext(const VkExtent2D& extent)
{
    if (g_contextReady)
        return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    g_iniPath = logx::dataDir() + "imgui.ini";
    io.IniFilename = g_iniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    float scale = std::max(1.0f, extent.height / 1080.0f);
    ImGui::StyleColorsDark();
    menu::initStyle(scale);

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_lastFrame);
    g_contextReady = true;
    LOG("ImGui %s context created, UI scale %.2f", IMGUI_VERSION, scale);
}

bool initBackend(DeviceData* d, VkQueue queue, uint32_t family, const Swapchain& sc)
{
    g_dev = d;
    g_queue = queue;
    g_queueFamily = family;
    g_renderPass = createRenderPass(d, sc.format);
    if (!g_renderPass) {
        LOG("render pass creation failed for format %d", sc.format);
        return false;
    }
    g_renderPassFormat = sc.format;

    initContext(sc.extent);
    if (!ImGui_ImplVulkan_LoadFunctions(d->instance->apiVersion, &imguiLoader)) {
        LOG("ImGui_ImplVulkan_LoadFunctions failed");
        d->DestroyRenderPass(d->device, g_renderPass, nullptr);
        g_renderPass = VK_NULL_HANDLE;
        return false;
    }

    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = d->instance->apiVersion;
    ii.Instance = d->instance->instance;
    ii.PhysicalDevice = d->physicalDevice;
    ii.Device = d->device;
    ii.QueueFamily = family;
    ii.Queue = queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 8;
    ii.RenderPass = g_renderPass;
    ii.MinImageCount = 2;
    ii.ImageCount = kImGuiRingSize;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        LOG("ImGui_ImplVulkan_Init failed");
        d->DestroyRenderPass(d->device, g_renderPass, nullptr);
        g_renderPass = VK_NULL_HANDLE;
        return false;
    }

    QueryPerformanceCounter(&g_initTime);
    g_backendReady = true;
    LOG("overlay backend ready: queue family %u, format %d", family, sc.format);
    return true;
}

void shutdownBackend()
{
    if (!g_backendReady)
        return;
    for (auto& [handle, sc] : g_swapchains)
        if (sc.dev == g_dev)
            releaseImages(sc);
    ImGui_ImplVulkan_Shutdown();
    g_dev->DestroyRenderPass(g_dev->device, g_renderPass, nullptr);
    g_renderPass = VK_NULL_HANDLE;
    g_renderPassFormat = VK_FORMAT_UNDEFINED;
    g_backendReady = false;
    g_dev = nullptr;
}

} // namespace

void onSwapchainCreated(DeviceData* d, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& ci)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Swapchain sc;
    sc.dev = d;
    sc.format = ci.imageFormat;
    sc.colorSpace = ci.imageColorSpace;
    sc.extent = ci.imageExtent;

    uint32_t count = 0;
    d->GetSwapchainImagesKHR(d->device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    d->GetSwapchainImagesKHR(d->device, swapchain, &count, images.data());
    for (VkImage img : images) {
        SwapImage im;
        im.image = img;
        sc.images.push_back(im);
    }
    g_swapchains[swapchain] = std::move(sc);
    LOG("swapchain created: %ux%u, format %d, colorspace %d, %u images, present mode %d", ci.imageExtent.width,
        ci.imageExtent.height, ci.imageFormat, ci.imageColorSpace, count, ci.presentMode);
}

void onSwapchainDestroyed(DeviceData*, VkSwapchainKHR swapchain)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end())
        return;
    releaseImages(it->second);
    g_swapchains.erase(it);
}

void onDeviceDestroyed(DeviceData* d)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_backendReady && g_dev == d)
        shutdownBackend();
    for (auto it = g_swapchains.begin(); it != g_swapchains.end();) {
        if (it->second.dev == d) {
            releaseImages(it->second);
            it = g_swapchains.erase(it);
        } else {
            ++it;
        }
    }
}

bool onPresent(DeviceData* d, VkQueue queue, uint32_t queueFamily, const VkPresentInfoKHR* info,
               VkSemaphore* doneSemaphore)
{
    if (info->swapchainCount != 1)
        return false;

    input::pollHotkey();
    gamepad::poll();
    cheats::tick();
    cheats::debugTick();
    const bool open = input::menuOpen();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(info->pSwapchains[0]);
    if (it == g_swapchains.end())
        return false;
    Swapchain& sc = it->second;

    if (queueFamily >= d->families.size() || !(d->families[queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        if (!g_warnedQueue) {
            LOG("present queue family %u has no graphics support, overlay off", queueFamily);
            g_warnedQueue = true;
        }
        return false;
    }

    if (g_backendReady && g_dev != d)
        return false;
    if (g_backendReady && (g_renderPassFormat != sc.format || g_queueFamily != queueFamily)) {
        LOG("swapchain format/queue changed, rebuilding overlay backend");
        shutdownBackend();
    }
    if (!g_backendReady && !initBackend(d, queue, queueFamily, sc))
        return false;

    const bool watermark = secondsSince(g_initTime) < kWatermarkSeconds;
    if (!open && !watermark)
        return false;

    if (!sc.built && !buildImages(sc))
        return false;

    const uint32_t index = info->pImageIndices[0];
    if (index >= sc.images.size())
        return false;
    SwapImage& im = sc.images[index];

    if (d->WaitForFences(d->device, 1, &im.fence, VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
        LOG("overlay fence timeout on image %u, skipping frame", index);
        return false;
    }

    // --- build the UI ---
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(sc.extent.width), float(sc.extent.height));
    io.DeltaTime = float(std::clamp(secondsSince(g_lastFrame), 1e-4, 0.1));
    QueryPerformanceCounter(&g_lastFrame);
    input::feedImGui(io, sc.extent);
    gamepad::feedImGui(io);

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    menu::FrameInfo fi{open, watermark, sc.extent.width, sc.extent.height, int(sc.format), int(sc.colorSpace)};
    menu::draw(fi);
    ImGui::Render();
    if (sc.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
        encodeHdr10(ImGui::GetDrawData());

    // --- record + submit ---
    d->ResetFences(d->device, 1, &im.fence);
    d->ResetCommandPool(d->device, im.pool, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(im.cmd, &bi);

    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = g_renderPass;
    rbi.framebuffer = im.framebuffer;
    rbi.renderArea.extent = sc.extent;
    d->CmdBeginRenderPass(im.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), im.cmd);
    d->CmdEndRenderPass(im.cmd);
    d->EndCommandBuffer(im.cmd);

    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &im.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &im.done;

    VkResult res = d->QueueSubmit(queue, 1, &si, im.fence);
    if (res != VK_SUCCESS) {
        LOG("overlay submit failed: %d", res);
        // The fence was reset and will never signal; replace it so the next frame can proceed.
        d->DestroyFence(d->device, im.fence, nullptr);
        im.fence = createSignaledFence(d);
        return false;
    }
    *doneSemaphore = im.done;
    return true;
}

} // namespace overlay
