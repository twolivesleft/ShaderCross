//
//  VulkanGlsl.hpp
//  ShaderCross
//
//  Adapts preprocessed GLSL written for OpenGL/Metal conventions to bgfx's Vulkan
//  resource layout, so the same shader source compiles to SPIR-V.
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ShaderCross
{
    /// Rewrites the preprocessed stages of one program for Vulkan: loose non-opaque uniforms, and
    /// bgfx built-ins they use but never declare, move into a uniform block (bgfx allows one per
    /// stage). The stages share that block, as a program links anonymous block members by name.
    /// Stages that already follow the rule come back unchanged.
    std::vector<std::string> adaptGlslForVulkan(const std::vector<std::string>& stages);

    /// Moves the uniform block to bgfx's binding for the stage (0 vertex, 1 fragment), splits
    /// combined image samplers and places every texture at binding `2 + stage` with its sampler
    /// at `18 + stage`, keeping the names the shader declared. Texture bindings written by hand
    /// are left alone.
    bool assignBgfxBindings(std::vector<uint32_t>& spirv, bool vertexStage, std::string* error = nullptr);
}
