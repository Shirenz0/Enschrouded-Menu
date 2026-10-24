#pragma once

#include "layer.h"

namespace overlay {

void onSwapchainCreated(DeviceData* d, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& ci);
void onSwapchainDestroyed(DeviceData* d, VkSwapchainKHR swapchain);
void onDeviceDestroyed(DeviceData* d);

// Draws the overlay into the image about to be presented. Returns true when it submitted work;
// the caller must then present waiting on *doneSemaphore instead of the app's semaphores.
bool onPresent(DeviceData* d, VkQueue queue, uint32_t queueFamily, const VkPresentInfoKHR* info,
               VkSemaphore* doneSemaphore);

} // namespace overlay
