#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

struct ImGuiIO;

namespace input {

// IAT hooks in enshrouded.exe (raw input, cursor clip/warp). Safe to call more than once.
void installHooks();

// Subclasses the game window found through vkCreateWin32SurfaceKHR.
void attachWindow(HWND hwnd);

// Edge-detects F2 while the game window has focus. Called once per present.
void pollHotkey();
bool menuOpen();

// Raw input HID reports read by the game since the last call (diagnostics).
uint32_t takeHidReads();
void toggleMenu();

// Mouse position/buttons/wheel for ImGui, mapped into swapchain pixels.
void feedImGui(ImGuiIO& io, const VkExtent2D& extent);

} // namespace input
