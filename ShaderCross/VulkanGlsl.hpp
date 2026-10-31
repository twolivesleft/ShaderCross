//
//  VulkanGlsl.hpp
//  ShaderCross
//
//  Compiles GLSL written for OpenGL/Metal conventions to SPIR-V in bgfx's Vulkan resource layout.
//

#pragma once

#include <glslang/glslang/Public/ShaderLang.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ShaderCross
{
    /// Has glslang gather the stage's loose uniforms into one uniform block, CodeaUniforms (bgfx allows
    /// one per stage). Linking merges the blocks of the stages that have one, by member name.
    void useBgfxUniformBlock(glslang::TShader& shader, EShLanguage stage);

    /// Declarations, for the preamble, of the bgfx built-ins the preprocessed source uses but does not
    /// declare. common.glsl declares them only for targets other than SPIR-V.
    std::string undeclaredBuiltins(const std::string& source);

    /// Moves the uniform block to bgfx's binding for the stage (0 vertex, 1 fragment), splits
    /// combined image samplers and places every texture at binding `2 + stage` with its sampler
    /// at `18 + stage`, keeping the names the shader declared. Texture bindings written by hand
    /// are left alone.
    bool assignBgfxBindings(std::vector<uint32_t>& spirv, bool vertexStage, std::string* error = nullptr);
}
