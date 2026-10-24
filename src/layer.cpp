// Vulkan implicit layer entry points. Everything not listed in the intercept tables goes straight
// to the next layer; the intercepts only record state and hand off to overlay/input.

#include "layer.h"

#include "gamepad.h"
#include "input.h"
#include "log.h"
#include "mods.h"
#include "overlay.h"
#include "steaminput.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace {

std::mutex g_mutex;
std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;

// Only enshrouded.exe gets the overlay; any other Vulkan app sees a pure pass-through layer.
bool g_active = false;

template <typename T> void* dispatchKey(T handle)
{
    return *reinterpret_cast<void**>(handle);
}

InstanceData* instanceData(void* key)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(key);
    return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData* deviceData(void* key)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(key);
    return it == g_devices.end() ? nullptr : it->second.get();
}

bool isEnshrouded()
{
    if (GetEnvironmentVariableA("ENSHROUDED_MENU_ANY_APP", nullptr, 0) > 0)
        return true;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    return _stricmp(name, "enshrouded.exe") == 0;
}

template <typename T> T* chainInfo(const void* pNext, VkStructureType type, VkLayerFunction func)
{
    auto* info = static_cast<T*>(const_cast<void*>(pNext));
    while (info && !(info->sType == type && info->function == func))
        info = static_cast<T*>(const_cast<void*>(info->pNext));
    return info;
}

// --- instance -----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci,
                                              const VkAllocationCallbacks* alloc,
                                              VkInstance* out)
{
    auto* link = chainInfo<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                                      VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult res = create(ci, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    auto data = std::make_unique<InstanceData>();
    data->instance = *out;
    if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion)
        data->apiVersion = ci->pApplicationInfo->apiVersion;
    data->GetInstanceProcAddr = gipa;
#define LOAD(name) data->name = reinterpret_cast<PFN_vk##name>(gipa(*out, "vk" #name))
    LOAD(DestroyInstance);
    LOAD(CreateWin32SurfaceKHR);
    LOAD(GetPhysicalDeviceQueueFamilyProperties);
#undef LOAD

    if (g_active) {
        logx::init();
        // Pin: our hooks (IAT, WndProc, XInput, Steam Input) outlive any VkInstance. When the loader
        // unloaded the layer on vkDestroyInstance, Steam jumped into freed code (crash 2026-10-24).
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&CreateInstance), &self);
        LOG("EnshroudedMenu.dll at %p (pinned)", self);
        LOG("vkCreateInstance ok, app apiVersion %u.%u.%u", VK_API_VERSION_MAJOR(data->apiVersion),
            VK_API_VERSION_MINOR(data->apiVersion), VK_API_VERSION_PATCH(data->apiVersion));
        input::installHooks();
        gamepad::install();
        steaminput::tryInstall(); // early, to see the game register its Steam Input event callback
        mods::skillTreeAvailable(); // records the config state the game started with
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances[dispatchKey(*out)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc)
{
    if (!instance)
        return;
    void* key = dispatchKey(instance);
    InstanceData* data = instanceData(key);
    if (!data)
        return;
    PFN_vkDestroyInstance destroy = data->DestroyInstance;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances.erase(key);
    }
    destroy(instance, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* ci,
                                                     const VkAllocationCallbacks* alloc, VkSurfaceKHR* out)
{
    InstanceData* data = instanceData(dispatchKey(instance));
    VkResult res = data->CreateWin32SurfaceKHR(instance, ci, alloc, out);
    if (res == VK_SUCCESS)
        input::attachWindow(ci->hwnd);
    return res;
}

// --- device -------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                            const VkAllocationCallbacks* alloc, VkDevice* out)
{
    auto* link = chainInfo<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                                    VK_LAYER_LINK_INFO);
    auto* loaderData = chainInfo<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                                          VK_LOADER_DATA_CALLBACK);
    if (!link || !link->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    InstanceData* inst = instanceData(dispatchKey(phys));
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst ? inst->instance : VK_NULL_HANDLE, "vkCreateDevice"));
    VkResult res = create(phys, ci, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    auto data = std::make_unique<DeviceData>();
    data->device = *out;
    data->physicalDevice = phys;
    data->instance = inst;
    data->GetDeviceProcAddr = gdpa;
    data->SetDeviceLoaderData = loaderData ? loaderData->u.pfnSetDeviceLoaderData : nullptr;

    if (inst && inst->GetPhysicalDeviceQueueFamilyProperties) {
        uint32_t count = 0;
        inst->GetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
        data->families.resize(count);
        inst->GetPhysicalDeviceQueueFamilyProperties(phys, &count, data->families.data());
    }

#define LOAD(name) data->name = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name))
    LOAD(DestroyDevice);
    LOAD(GetDeviceQueue);
    LOAD(GetDeviceQueue2);
    LOAD(CreateSwapchainKHR);
    LOAD(DestroySwapchainKHR);
    LOAD(GetSwapchainImagesKHR);
    LOAD(QueuePresentKHR);
    LOAD(QueueSubmit);
    LOAD(CreateImageView);
    LOAD(DestroyImageView);
    LOAD(CreateFramebuffer);
    LOAD(DestroyFramebuffer);
    LOAD(CreateRenderPass);
    LOAD(DestroyRenderPass);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(ResetCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdBeginRenderPass);
    LOAD(CmdEndRenderPass);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(CreateSemaphore);
    LOAD(DestroySemaphore);
#undef LOAD

    if (g_active)
        LOG("vkCreateDevice ok, %u queue families, loader callback %s", (unsigned)data->families.size(),
            data->SetDeviceLoaderData ? "yes" : "NO");

    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[dispatchKey(*out)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc)
{
    if (!device)
        return;
    void* key = dispatchKey(device);
    DeviceData* data = deviceData(key);
    if (!data)
        return;
    if (g_active)
        overlay::onDeviceDestroyed(data);
    PFN_vkDestroyDevice destroy = data->DestroyDevice;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices.erase(key);
    }
    destroy(device, alloc);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out)
{
    DeviceData* data = deviceData(dispatchKey(device));
    data->GetDeviceQueue(device, family, index, out);
    if (*out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        data->queueFamily[*out] = family;
    }
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* out)
{
    DeviceData* data = deviceData(dispatchKey(device));
    data->GetDeviceQueue2(device, info, out);
    if (*out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        data->queueFamily[*out] = info->queueFamilyIndex;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                  const VkAllocationCallbacks* alloc, VkSwapchainKHR* out)
{
    DeviceData* data = deviceData(dispatchKey(device));
    // The overlay renders straight into the swapchain images.
    VkSwapchainCreateInfoKHR patched = *ci;
    patched.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkResult res = data->CreateSwapchainKHR(device, &patched, alloc, out);
    if (res == VK_SUCCESS)
        overlay::onSwapchainCreated(data, *out, patched);
    else
        LOG("vkCreateSwapchainKHR failed: %d", res);
    return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks* alloc)
{
    DeviceData* data = deviceData(dispatchKey(device));
    if (swapchain)
        overlay::onSwapchainDestroyed(data, swapchain);
    data->DestroySwapchainKHR(device, swapchain, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
{
    DeviceData* data = deviceData(dispatchKey(queue));
    uint32_t family = UINT32_MAX;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = data->queueFamily.find(queue);
        if (it != data->queueFamily.end())
            family = it->second;
    }

    VkSemaphore overlayDone = VK_NULL_HANDLE;
    if (overlay::onPresent(data, queue, family, info, &overlayDone)) {
        VkPresentInfoKHR patched = *info;
        patched.waitSemaphoreCount = 1;
        patched.pWaitSemaphores = &overlayDone;
        return data->QueuePresentKHR(queue, &patched);
    }
    return data->QueuePresentKHR(queue, info);
}

// --- proc addr ----------------------------------------------------------------------------

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name);

PFN_vkVoidFunction deviceIntercept(const char* name)
{
#define HOOK(fn)                                                                                  \
    if (strcmp(name, "vk" #fn) == 0)                                                              \
        return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    HOOK(GetDeviceProcAddr);
    HOOK(DestroyDevice);
    if (!g_active)
        return nullptr;
    HOOK(GetDeviceQueue);
    HOOK(GetDeviceQueue2);
    HOOK(CreateSwapchainKHR);
    HOOK(DestroySwapchainKHR);
    HOOK(QueuePresentKHR);
#undef HOOK
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name);

PFN_vkVoidFunction instanceIntercept(const char* name)
{
#define HOOK(fn)                                                                                  \
    if (strcmp(name, "vk" #fn) == 0)                                                              \
        return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    HOOK(GetInstanceProcAddr);
    HOOK(CreateInstance);
    HOOK(DestroyInstance);
    HOOK(CreateDevice);
    if (!g_active)
        return nullptr;
    HOOK(CreateWin32SurfaceKHR);
#undef HOOK
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name)
{
    if (PFN_vkVoidFunction fn = deviceIntercept(name))
        return fn;
    DeviceData* data = device ? deviceData(dispatchKey(device)) : nullptr;
    return data ? data->GetDeviceProcAddr(device, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name)
{
    if (PFN_vkVoidFunction fn = instanceIntercept(name))
        return fn;
    if (PFN_vkVoidFunction fn = deviceIntercept(name))
        return fn;
    InstanceData* data = instance ? instanceData(dispatchKey(instance)) : nullptr;
    return data ? data->GetInstanceProcAddr(instance, name) : nullptr;
}

} // namespace

VkResult layerAllocateCommandBuffers(DeviceData* d, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out)
{
    VkResult res = d->AllocateCommandBuffers(d->device, info, out);
    if (res == VK_SUCCESS && d->SetDeviceLoaderData) {
        for (uint32_t i = 0; i < info->commandBufferCount; ++i)
            d->SetDeviceLoaderData(d->device, out[i]);
    }
    return res;
}

// --- exports (names mapped in exports.def) ------------------------------------------------

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Layer_GetInstanceProcAddr(VkInstance instance, const char* name)
{
    return GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Layer_GetDeviceProcAddr(VkDevice device, const char* name)
{
    return GetDeviceProcAddr(device, name);
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* p)
{
    if (!p || p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (p->loaderLayerInterfaceVersion >= 2) {
        p->pfnGetInstanceProcAddr = &Layer_GetInstanceProcAddr;
        p->pfnGetDeviceProcAddr = &Layer_GetDeviceProcAddr;
        p->pfnGetPhysicalDeviceProcAddr = nullptr;
        p->loaderLayerInterfaceVersion = 2;
    }
    return VK_SUCCESS;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    // Loader lock: only cheap calls here, logging starts at the first vkCreateInstance.
    if (reason == DLL_PROCESS_ATTACH)
        g_active = isEnshrouded();
    return TRUE;
}
