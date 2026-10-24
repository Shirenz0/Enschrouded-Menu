#pragma once

#include <cstdint>

namespace menu {

struct FrameInfo {
    bool open;      // F2 menu visible
    bool watermark; // short "loaded" toast after start
    uint32_t width;
    uint32_t height;
    int format;
    int colorSpace;
};

// Loads fonts and sets the ImGui style. Call once after ImGui::CreateContext().
void initStyle(float scale);

// Builds this frame's ImGui windows. Called between ImGui::NewFrame() and ImGui::Render().
void draw(const FrameInfo& fi);

} // namespace menu
