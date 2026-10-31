//
//  VulkanGlsl.cpp
//  ShaderCross
//

#include "VulkanGlsl.hpp"

#include <glslang/glslang/Public/ShaderLang.h>
#include <spirv-tools/optimizer.hpp>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ShaderCross
{
    namespace
    {
        // bgfx's Vulkan layout: textures at kSpirvBindShift + stage, samplers kSpirvSamplerShift above
        constexpr uint32_t kTextureBindingShift = 2;
        constexpr uint32_t kSamplerBindingShift = 16;
        constexpr uint32_t kMaxTextureStages = 16;
        // bgfx binds each stage's uniform buffer at its own binding
        constexpr uint32_t kVertexUniformBinding = 0;
        constexpr uint32_t kFragmentUniformBinding = 1;

        // bgfx and Codea uniforms common.glsl declares only for non-SPIR-V targets
        // A shader may also declare a built-in sampler as a separate texture, under the same name
        struct Builtin { const char* name; const char* type; const char* array; const char* textureType; };
        const Builtin kBuiltins[] =
        {
            {"u_viewRect", "vec4", "", ""},
            {"u_viewTexel", "vec4", "", ""},
            {"u_view", "mat4", "", ""},
            {"u_invView", "mat4", "", ""},
            {"u_proj", "mat4", "", ""},
            {"u_invProj", "mat4", "", ""},
            {"u_viewProj", "mat4", "", ""},
            {"u_invViewProj", "mat4", "", ""},
            {"u_model", "mat4", "[128]", ""},
            {"u_modelView", "mat4", "", ""},
            {"u_modelViewProj", "mat4", "", ""},
            {"u_alphaRef4", "vec4", "", ""},
            {"u_normal", "mat3", "", ""},
            {"u_cameraPosition", "vec3", "", ""},
            {"u_time", "vec4", "", ""},
            {"u_tintColor", "vec4", "", ""},
            {"s_texture", "sampler2D", "", "texture2D"},
            {"s_curveAtlasTexture", "sampler2D", "", "texture2D"},
        };

        bool isIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        std::string wordBefore(const std::string& s, size_t at)
        {
            size_t end = at;
            while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
            size_t begin = end;
            while (begin > 0 && isIdentifierChar(s[begin - 1])) begin--;
            return s.substr(begin, end - begin);
        }
    }

    void useBgfxUniformBlock(glslang::TShader& shader, EShLanguage stage)
    {
        shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        shader.setEnvInputVulkanRulesRelaxed();
        shader.setGlobalUniformBlockName("CodeaUniforms");
        // Linking requires the stages' blocks to agree; assignBgfxBindings moves each to its stage's binding
        shader.setGlobalUniformBinding(kVertexUniformBinding);
        shader.setGlobalUniformSet(0);
    }

    std::string undeclaredBuiltins(const std::string& source)
    {
        std::string declarations;
        for (const Builtin& builtin : kBuiltins)
        {
            std::string name = builtin.name;
            bool used = false, declared = false;
            for (size_t at = source.find(name); at != std::string::npos; at = source.find(name, at + 1))
            {
                size_t end = at + name.size();
                if ((at > 0 && isIdentifierChar(source[at - 1])) || (end < source.size() && isIdentifierChar(source[end]))) continue;
                used = true;
                std::string type = wordBefore(source, at);
                declared |= type == builtin.type || (*builtin.textureType && type == builtin.textureType);
            }
            if (used && !declared) declarations += "uniform " + std::string(builtin.type) + " " + name + builtin.array + ";\n";
        }
        return declarations;
    }

    namespace
    {
        enum : uint32_t
        {
            OpName = 5, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28,
            OpTypePointer = 32, OpVariable = 59, OpDecorate = 71, DecorationBinding = 33,
            StorageClassUniform = 2,
        };

        bool endsWith(const std::string& s, const char* suffix)
        {
            size_t n = std::strlen(suffix);
            return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
        }
    }

    bool assignBgfxBindings(std::vector<uint32_t>& spirv, bool vertexStage, std::string* error)
    {
        auto forEachInstruction = [&](auto&& visit)
        {
            for (size_t i = 5; i < spirv.size(); )
            {
                uint32_t count = spirv[i] >> 16;
                if (count == 0) break;
                visit(i, spirv[i] & 0xFFFF, count);
                i += count;
            }
        };

        bool hasCombined = false;
        forEachInstruction([&](size_t, uint32_t op, uint32_t) { hasCombined |= op == OpTypeSampledImage; });

        if (hasCombined)
        {
            spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_0);
            optimizer.RegisterPass(spvtools::CreateSplitCombinedImageSamplerPass());
            std::vector<uint32_t> split;
            if (!optimizer.Run(spirv.data(), spirv.size(), &split))
            {
                if (error) *error = "Could not split combined image samplers";
                return false;
            }
            spirv = std::move(split);
        }

        // What each resource variable is, and what it is called
        std::map<uint32_t, uint32_t> typeKind;        // type id -> OpTypeImage / OpTypeSampler
        std::map<uint32_t, uint32_t> pointerKind;     // pointer type id -> pointee kind
        std::map<uint32_t, uint32_t> variableKind;    // variable id -> kind
        std::map<uint32_t, std::string> names;
        std::map<uint32_t, size_t> bindingWord;       // variable id -> index of its Binding operand
        std::set<uint32_t> uniformBlocks;
        forEachInstruction([&](size_t i, uint32_t op, uint32_t)
        {
            switch (op)
            {
                case OpTypeImage: case OpTypeSampler: typeKind[spirv[i + 1]] = op; break;
                case OpTypeArray: if (typeKind.count(spirv[i + 2])) typeKind[spirv[i + 1]] = typeKind[spirv[i + 2]]; break;
                case OpTypePointer: if (typeKind.count(spirv[i + 3])) pointerKind[spirv[i + 1]] = typeKind[spirv[i + 3]]; break;
                case OpVariable:
                    if (pointerKind.count(spirv[i + 1])) variableKind[spirv[i + 2]] = pointerKind[spirv[i + 1]];
                    else if (spirv[i + 3] == StorageClassUniform) uniformBlocks.insert(spirv[i + 2]);
                    break;
                case OpName: names[spirv[i + 1]] = reinterpret_cast<const char*>(&spirv[i + 2]); break;
                case OpDecorate: if (spirv[i + 2] == DecorationBinding) bindingWord[spirv[i + 1]] = i + 3; break;
            }
        });

        for (uint32_t id : uniformBlocks)
        {
            if (bindingWord.count(id)) spirv[bindingWord[id]] = vertexStage ? kVertexUniformBinding : kFragmentUniformBinding;
        }

        // Bindings written by hand keep their stage; the split pairs share the rest
        std::set<uint32_t> usedStages;
        std::map<std::string, std::pair<uint32_t, uint32_t>> pairs;   // base name -> image, sampler ids
        for (const auto& [id, kind] : variableKind)
        {
            const std::string& name = names[id];
            if (kind == OpTypeImage && endsWith(name, "_image")) pairs[name.substr(0, name.size() - 6)].first = id;
            else if (kind == OpTypeSampler && endsWith(name, "_sampler") && bindingWord.count(id)) pairs[name.substr(0, name.size() - 8)].second = id;
            else if (bindingWord.count(id))
            {
                uint32_t binding = spirv[bindingWord[id]];
                uint32_t shift = kind == OpTypeSampler ? kTextureBindingShift + kSamplerBindingShift : kTextureBindingShift;
                if (binding >= shift) usedStages.insert(binding - shift);
            }
        }

        std::map<uint32_t, std::string> renames;
        for (const auto& [name, ids] : pairs)
        {
            if (!ids.first || !ids.second || !bindingWord.count(ids.first)) continue;
            uint32_t stage = kMaxTextureStages;
            for (uint32_t i = 0; i < kMaxTextureStages; i++)
            {
                // Vertex textures count down from the top so the two stages rarely share a slot
                uint32_t candidate = vertexStage ? kMaxTextureStages - 1 - i : i;
                if (usedStages.insert(candidate).second) { stage = candidate; break; }
            }
            if (stage == kMaxTextureStages)
            {
                if (error) *error = "Too many textures for one shader stage";
                return false;
            }
            spirv[bindingWord[ids.first]] = kTextureBindingShift + stage;
            spirv[bindingWord[ids.second]] = kTextureBindingShift + kSamplerBindingShift + stage;
            renames[ids.first] = name;
        }

        if (renames.empty()) return true;

        // bgfx and the engine look textures up by the name the shader declared
        std::vector<uint32_t> result(spirv.begin(), spirv.begin() + 5);
        forEachInstruction([&](size_t i, uint32_t op, uint32_t count)
        {
            if (op == OpName && renames.count(spirv[i + 1]))
            {
                const std::string& name = renames[spirv[i + 1]];
                std::vector<uint32_t> literal((name.size() + 4) / 4, 0);
                std::memcpy(literal.data(), name.data(), name.size());
                result.push_back(uint32_t((2 + literal.size()) << 16) | OpName);
                result.push_back(spirv[i + 1]);
                result.insert(result.end(), literal.begin(), literal.end());
                return;
            }
            result.insert(result.end(), spirv.begin() + i, spirv.begin() + i + count);
        });
        spirv = std::move(result);
        return true;
    }
}
