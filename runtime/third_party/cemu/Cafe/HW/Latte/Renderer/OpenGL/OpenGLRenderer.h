// Decompiler API selection shim for the OpenGL renderer (gfx/gl): GLSL without the VULKAN define.
#pragma once
#include "Cafe/HW/Latte/Renderer/Renderer.h"
class OpenGLRenderer : public Renderer {
public:
    OpenGLRenderer() : Renderer(RendererAPI::OpenGL) {}
};
