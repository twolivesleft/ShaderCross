#include <mutex>
//
//  ShaderCross.cpp
//  ShaderCross
//
//  Created by John Millard on 18/6/20.
//  Copyright © 2020 John Millard. All rights reserved.
//
// Shader cross compiler used by Codea (Carbide)
//
// Originally based on the Krafix shader compiler (https://github.com/Kode/krafix)

#include "ShaderCross.hpp"
// Builds that compile SPIR-V for bgfx's Vulkan renderer also build VulkanGlsl.cpp and link SPIRV-Tools' optimizer
#ifdef SHADERCROSS_BGFX_VULKAN
#include "VulkanGlsl.hpp"
#endif

#include <glslang/StandAlone/ResourceLimits.h>
#include <glslang/StandAlone/Worklist.h>
#include <glslang/glslang/Include/ShHandle.h>
#include <glslang/glslang/Include/revision.h>
#include <glslang/glslang/Public/ShaderLang.h>
#include "MetalBindingResolver.hpp"
// Include SPIRV-Cross before glslang/SPIRV/doc.h: both spirv.hpp and spirv.hpp11 use
// the same include guard (spirv_HPP). We need the spirv-cross enum-based API to win.
#include <SPIRV-Cross/spirv_parser.hpp>
#include <SPIRV-Cross/spirv_reflect.hpp>
#include <SPIRV-Cross/spirv_common.hpp>

#include <glslang/SPIRV/GlslangToSpv.h>

#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/SPIRV/GLSL.std.450.h>
#include <glslang/SPIRV/doc.h>
#include <glslang/SPIRV/disassemble.h>
#include <glslang/OSDependent/osinclude.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>
#include <cctype>
#include <cmath>
#include <array>
#include <memory>
#include <sstream>
#include <fstream>

#include "SpirVTranslator.h"
#include "GlslTranslator2.h"
#include "HlslTranslator2.h"
#include "AgalTranslator.h"
#include "MetalTranslator2.h"
#include "VarListTranslator.h"

namespace ShaderCross
{
    namespace
    {
        // bgfx binds vertex inputs by name, in bgfx::Attrib order. Codea's POSITION, NORMAL,
        // COLOR0, TEXCOORD0... location defines (common.glsl) use the same order.
        const char* const kBgfxAttribNames[] =
        {
            "a_position", "a_normal", "a_tangent", "a_bitangent",
            "a_color0", "a_color1", "a_color2", "a_color3",
            "a_indices", "a_weight",
            "a_texcoord0", "a_texcoord1", "a_texcoord2", "a_texcoord3",
            "a_texcoord4", "a_texcoord5", "a_texcoord6", "a_texcoord7",
            "a_texcoord8", "a_texcoord9", "a_texcoord10", "a_texcoord11",
            "a_texcoord12", "a_texcoord13", "a_texcoord14", "a_texcoord15",
        };

        const size_t kBgfxAttribCount = sizeof(kBgfxAttribNames) / sizeof(kBgfxAttribNames[0]);
        const int kInstanceData = -1, kUnknownInput = -2;

        // The bgfx attribute an input's name selects, kInstanceData, or kUnknownInput
        int bgfxAttribForName(const std::string& name)
        {
            if (name.compare(0, 6, "i_data") == 0) return kInstanceData;
            for (size_t attrib = 0; attrib < kBgfxAttribCount; attrib++)
                if (name == kBgfxAttribNames[attrib]) return (int)attrib;
            return kUnknownInput;
        }

        void appendOpName(std::vector<unsigned int>& spirv, uint32_t target, const std::string& name)
        {
            constexpr uint32_t OpName = 5;
            const uint32_t stringWords = (uint32_t)(name.size() + 4) / 4; // includes the terminator
            const size_t at = spirv.size();
            spirv.resize(at + 2 + stringWords, 0);
            spirv[at] = ((2 + stringWords) << 16) | OpName;
            spirv[at + 1] = target;
            std::memcpy(&spirv[at + 2], name.data(), name.size());
        }
    }

    /// Names each vertex input after the bgfx attribute its location selects, so
    /// `layout(location = COLOR0) in vec4 a_color;` reads the mesh's colors. Inputs already
    /// named for a bgfx attribute are left alone. Fails, with a message for the shader's
    /// author, for an input bgfx could not bind; the renderer would reject it at draw time.
    /// Uses id-indexed vectors rather than std::map: this file builds as C++14 while the engine
    /// builds as C++20, and libc++'s map lookups differ between the two under the same symbols.
    static bool assignVertexInputNames(std::vector<unsigned int>& spirv, std::string& error)
    {
        constexpr uint32_t OpName = 5, OpVariable = 59, OpDecorate = 71;
        constexpr uint32_t DecorationBuiltIn = 11, DecorationLocation = 30, StorageClassInput = 1;
        constexpr uint32_t kNoLocation = UINT32_MAX;

        if (spirv.size() < 5) return true;
        const uint32_t bound = spirv[3];
        std::vector<std::string> names(bound);
        std::vector<bool> named(bound, false), builtIn(bound, false);
        std::vector<uint32_t> locations(bound, kNoLocation);
        std::vector<uint32_t> inputs;
        size_t firstDecoration = spirv.size();

        auto valid = [&](uint32_t id) { return id < bound; };
        for (size_t i = 5; i < spirv.size(); )
        {
            const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xFFFF;
            if (count == 0 || i + count > spirv.size()) return true; // malformed; the translator reports it
            if (op == OpName && count >= 3 && valid(spirv[i + 1]))
            {
                names[spirv[i + 1]] = reinterpret_cast<const char*>(&spirv[i + 2]);
                named[spirv[i + 1]] = true;
            }
            else if (op == OpDecorate && count >= 3 && valid(spirv[i + 1]))
            {
                firstDecoration = std::min(firstDecoration, i);
                if (spirv[i + 2] == DecorationLocation && count >= 4) locations[spirv[i + 1]] = spirv[i + 3];
                else if (spirv[i + 2] == DecorationBuiltIn) builtIn[spirv[i + 1]] = true;
            }
            else if (op == OpVariable && count >= 4 && spirv[i + 3] == StorageClassInput && valid(spirv[i + 2]))
            {
                inputs.push_back(spirv[i + 2]);
            }
            i += count;
        }

        std::vector<std::string> renames(bound);
        std::vector<std::string> claimedBy(kBgfxAttribCount); // the input reading each attribute
        bool renamed = false;
        for (uint32_t id : inputs)
        {
            if (builtIn[id]) continue;
            const std::string& name = names[id];
            int attrib = bgfxAttribForName(name);
            if (attrib == kInstanceData) continue;
            if (attrib == kUnknownInput)
            {
                if (locations[id] >= kBgfxAttribCount)
                {
                    error = "Vertex input '" + name + "' does not match a mesh attribute. Declare it with "
                            "layout(location = POSITION, NORMAL, TANGENT, COLOR0 or TEXCOORD0...), or name it "
                            "a_position, a_normal, a_tangent, a_color0 or a_texcoord0...";
                    return false;
                }
                attrib = (int)locations[id];
                renames[id] = kBgfxAttribNames[attrib];
                renamed = true;
            }
            if (!claimedBy[attrib].empty())
            {
                error = "Vertex inputs '" + claimedBy[attrib] + "' and '" + name + "' both read the mesh attribute " +
                        kBgfxAttribNames[attrib] + ". Give each input its own location.";
                return false;
            }
            claimedBy[attrib] = name;
        }
        if (!renamed) return true;

        std::vector<unsigned int> patched(spirv.begin(), spirv.begin() + 5);
        patched.reserve(spirv.size() + inputs.size() * 6);
        for (size_t i = 5; i < spirv.size(); )
        {
            const uint32_t count = spirv[i] >> 16;
            if (i == firstDecoration)
            {
                // Inputs glslang left unnamed get a name ahead of the annotations
                for (uint32_t id : inputs)
                    if (!renames[id].empty() && !named[id]) appendOpName(patched, id, renames[id]);
            }
            if ((spirv[i] & 0xFFFF) == OpName && valid(spirv[i + 1]) && !renames[spirv[i + 1]].empty())
                appendOpName(patched, spirv[i + 1], renames[spirv[i + 1]]);
            else
                patched.insert(patched.end(), spirv.begin() + i, spirv.begin() + i + count);
            i += count;
        }
        spirv = std::move(patched);
        return true;
    }

    static TBuiltInResource InitResources()
    {
        TBuiltInResource Resources;

        Resources.maxLights                                 = 32;
        Resources.maxClipPlanes                             = 6;
        Resources.maxTextureUnits                           = 32;
        Resources.maxTextureCoords                          = 32;
        Resources.maxVertexAttribs                          = 64;
        Resources.maxVertexUniformComponents                = 4096;
        Resources.maxVaryingFloats                          = 64;
        Resources.maxVertexTextureImageUnits                = 32;
        Resources.maxCombinedTextureImageUnits              = 80;
        Resources.maxTextureImageUnits                      = 32;
        Resources.maxFragmentUniformComponents              = 4096;
        Resources.maxDrawBuffers                            = 32;
        Resources.maxVertexUniformVectors                   = 128;
        Resources.maxVaryingVectors                         = 8;
        Resources.maxFragmentUniformVectors                 = 16;
        Resources.maxVertexOutputVectors                    = 16;
        Resources.maxFragmentInputVectors                   = 15;
        Resources.minProgramTexelOffset                     = -8;
        Resources.maxProgramTexelOffset                     = 7;
        Resources.maxClipDistances                          = 8;
        Resources.maxComputeWorkGroupCountX                 = 65535;
        Resources.maxComputeWorkGroupCountY                 = 65535;
        Resources.maxComputeWorkGroupCountZ                 = 65535;
        Resources.maxComputeWorkGroupSizeX                  = 1024;
        Resources.maxComputeWorkGroupSizeY                  = 1024;
        Resources.maxComputeWorkGroupSizeZ                  = 64;
        Resources.maxComputeUniformComponents               = 1024;
        Resources.maxComputeTextureImageUnits               = 16;
        Resources.maxComputeImageUniforms                   = 8;
        Resources.maxComputeAtomicCounters                  = 8;
        Resources.maxComputeAtomicCounterBuffers            = 1;
        Resources.maxVaryingComponents                      = 60;
        Resources.maxVertexOutputComponents                 = 64;
        Resources.maxGeometryInputComponents                = 64;
        Resources.maxGeometryOutputComponents               = 128;
        Resources.maxFragmentInputComponents                = 128;
        Resources.maxImageUnits                             = 8;
        Resources.maxCombinedImageUnitsAndFragmentOutputs   = 8;
        Resources.maxCombinedShaderOutputResources          = 8;
        Resources.maxImageSamples                           = 0;
        Resources.maxVertexImageUniforms                    = 0;
        Resources.maxTessControlImageUniforms               = 0;
        Resources.maxTessEvaluationImageUniforms            = 0;
        Resources.maxGeometryImageUniforms                  = 0;
        Resources.maxFragmentImageUniforms                  = 8;
        Resources.maxCombinedImageUniforms                  = 8;
        Resources.maxGeometryTextureImageUnits              = 16;
        Resources.maxGeometryOutputVertices                 = 256;
        Resources.maxGeometryTotalOutputComponents          = 1024;
        Resources.maxGeometryUniformComponents              = 1024;
        Resources.maxGeometryVaryingComponents              = 64;
        Resources.maxTessControlInputComponents             = 128;
        Resources.maxTessControlOutputComponents            = 128;
        Resources.maxTessControlTextureImageUnits           = 16;
        Resources.maxTessControlUniformComponents           = 1024;
        Resources.maxTessControlTotalOutputComponents       = 4096;
        Resources.maxTessEvaluationInputComponents          = 128;
        Resources.maxTessEvaluationOutputComponents         = 128;
        Resources.maxTessEvaluationTextureImageUnits        = 16;
        Resources.maxTessEvaluationUniformComponents        = 1024;
        Resources.maxTessPatchComponents                    = 120;
        Resources.maxPatchVertices                          = 32;
        Resources.maxTessGenLevel                           = 64;
        Resources.maxViewports                              = 16;
        Resources.maxVertexAtomicCounters                   = 0;
        Resources.maxTessControlAtomicCounters              = 0;
        Resources.maxTessEvaluationAtomicCounters           = 0;
        Resources.maxGeometryAtomicCounters                 = 0;
        Resources.maxFragmentAtomicCounters                 = 8;
        Resources.maxCombinedAtomicCounters                 = 8;
        Resources.maxAtomicCounterBindings                  = 1;
        Resources.maxVertexAtomicCounterBuffers             = 0;
        Resources.maxTessControlAtomicCounterBuffers        = 0;
        Resources.maxTessEvaluationAtomicCounterBuffers     = 0;
        Resources.maxGeometryAtomicCounterBuffers           = 0;
        Resources.maxFragmentAtomicCounterBuffers           = 1;
        Resources.maxCombinedAtomicCounterBuffers           = 1;
        Resources.maxAtomicCounterBufferSize                = 16384;
        Resources.maxTransformFeedbackBuffers               = 4;
        Resources.maxTransformFeedbackInterleavedComponents = 64;
        Resources.maxCullDistances                          = 8;
        Resources.maxCombinedClipAndCullDistances           = 8;
        Resources.maxSamples                                = 4;
        Resources.maxMeshOutputVerticesNV                   = 256;
        Resources.maxMeshOutputPrimitivesNV                 = 512;
        Resources.maxMeshWorkGroupSizeX_NV                  = 32;
        Resources.maxMeshWorkGroupSizeY_NV                  = 1;
        Resources.maxMeshWorkGroupSizeZ_NV                  = 1;
        Resources.maxTaskWorkGroupSizeX_NV                  = 32;
        Resources.maxTaskWorkGroupSizeY_NV                  = 1;
        Resources.maxTaskWorkGroupSizeZ_NV                  = 1;
        Resources.maxMeshViewCountNV                        = 4;

        Resources.limits.nonInductiveForLoops                 = 1;
        Resources.limits.whileLoops                           = 1;
        Resources.limits.doWhileLoops                         = 1;
        Resources.limits.generalUniformIndexing               = 1;
        Resources.limits.generalAttributeMatrixVectorIndexing = 1;
        Resources.limits.generalVaryingIndexing               = 1;
        Resources.limits.generalSamplerIndexing               = 1;
        Resources.limits.generalVariableIndexing              = 1;
        Resources.limits.generalConstantMatrixVectorIndexing  = 1;

        return Resources;
    }

    


    enum TOptions
    {
        EOptionNone = 0,
        EOptionIntermediate = (1 << 0),
        EOptionSuppressInfolog = (1 << 1),
        EOptionMemoryLeakMode = (1 << 2),
        EOptionRelaxedErrors = (1 << 3),
        EOptionGiveWarnings = (1 << 4),
        EOptionLinkProgram = (1 << 5),
        EOptionMultiThreaded = (1 << 6),
        EOptionDumpConfig = (1 << 7),
        EOptionDumpReflection = (1 << 8),
        EOptionSuppressWarnings = (1 << 9),
        EOptionDumpVersions = (1 << 10),
        EOptionSpv = (1 << 11),
        EOptionHumanReadableSpv = (1 << 12),
        EOptionVulkanRules = (1 << 13),
        EOptionDefaultDesktop = (1 << 14),
        EOptionOutputPreprocessed = (1 << 15),
        EOptionOutputHexadecimal = (1 << 16),
        EOptionReadHlsl = (1 << 17),
        EOptionCascadingErrors = (1 << 18),
        EOptionAutoMapBindings = (1 << 19),
        EOptionFlattenUniformArrays = (1 << 20),
        EOptionNoStorageFormat = (1 << 21),
        EOptionKeepUncalled = (1 << 22),
    };

    class CustomIncluder : public glslang::TShader::Includer
    {
    public:
        
        CustomIncluder(IncludeCallback callback) : m_callback(callback)
        {
            
        }
        
        IncludeResult* include(const char* headerName, const char* includerName, size_t inclusionDepth, bool local) {
            
            auto content = m_callback(headerName, local);
            std::string filecontent = content.second;
            char* heapcontent = new char[filecontent.size() + 1];
            strcpy(heapcontent, filecontent.c_str());
            return new IncludeResult(content.first, heapcontent, content.second.size(), heapcontent);
        }
        
        IncludeResult* includeSystem(const char* headerName, const char* includerName, size_t inclusionDepth) override {
            return include(headerName, includerName, inclusionDepth, false);
        }

        IncludeResult* includeLocal(const char* headerName, const char* includerName, size_t inclusionDepth) override {
            return include(headerName, includerName, inclusionDepth, true);
        }
        
        void releaseInclude(IncludeResult* result) override {
            if (result)
            {
                delete (char*)result->userData;
                delete result;
            }
        }
        
    private:
        IncludeCallback m_callback;
    };

    class KrafixIncluder : public glslang::TShader::Includer
    {
    public:
        KrafixIncluder(std::string from) {
            dir = from;
        }

        IncludeResult* includeSystem(const char* headerName, const char* includerName, size_t inclusionDepth) override {
            return includeLocal(headerName, includerName, inclusionDepth);
        }

        IncludeResult* includeLocal(const char* headerName, const char* includerName, size_t inclusionDepth) override {
            std::string realfilename = dir + headerName;
            std::stringstream content;
            std::string line;
            std::ifstream file(realfilename);
            if (file.is_open()) {
                while (getline(file, line)) {
                    content << line << '\n';
                }
                file.close();
            }
            std::string filecontent = content.str();
            char* heapcontent = new char[filecontent.size() + 1];
            strcpy(heapcontent, filecontent.c_str());
            return new IncludeResult(realfilename, heapcontent, content.str().size(), heapcontent);
        }

        void releaseInclude(IncludeResult* result) override {
            if (result)
            {
                delete (char*)result->userData;
                delete result;
            }
        }
    private:
        std::string dir;
    };

    class NullIncluder : public glslang::TShader::Includer {
    public:
        NullIncluder() {

        }

        IncludeResult* includeSystem(const char* headerName, const char* includerName, size_t inclusionDepth) override {
            return includeLocal(headerName, includerName, inclusionDepth);
        }

        IncludeResult* includeLocal(const char* headerName, const char* includerName, size_t inclusionDepth) override {
            return nullptr;
        }

        void releaseInclude(IncludeResult* result) override {

        }
    };

    // Simple bundling of what makes a compilation unit for ease in passing around,
    // and separation of handling file IO versus API (programmatic) compilation.
    struct ShaderCompUnit
    {
        EShLanguage stage;
        std::string fileName;
        char** text;             // memory owned/managed externally
        const char* fileNameList[1];

        // Need to have a special constructors to adjust the fileNameList, since back end needs a list of ptrs
        ShaderCompUnit(EShLanguage istage, std::string& ifileName, char** itext)
        {
            stage = istage;
            fileName = ifileName;
            text = itext;
            fileNameList[0] = fileName.c_str();
        }

        ShaderCompUnit(const ShaderCompUnit& rhs)
        {
            stage = rhs.stage;
            fileName = rhs.fileName;
            text = rhs.text;
            fileNameList[0] = fileName.c_str();
        }

    };



    ShaderStage shLanguageToShaderStage(EShLanguage lang)
    {
        switch (lang) {
        case EShLangVertex: return StageVertex;
        case EShLangTessControl: return StageTessControl;
        case EShLangTessEvaluation: return StageTessEvaluation;
        case EShLangGeometry: return StageGeometry;
        case EShLangFragment: return StageFragment;
        case EShLangCompute: return StageCompute;
        case EShLangCount:
        default:
            return StageCompute;
        }
    }

    void CompileAndLinkShaderUnits(const Config& config,
                                   Result& result,
                                   std::vector<ShaderCompUnit> compUnits,
                                   Target target,
                                   const char* sourcefilename,
                                   const char* filename,
                                   glslang::TShader::Includer& includer,
                                   const char* defines)
    {
        std::vector<char> compilerOutputBuffer(1024*1024);
        // keep track of what to free
        std::list<glslang::TShader*> shaders;

        EShMessages messages = EShMsgDefault;

        //
        // Per-shader processing...
        //
        
        bool linkFailed = false;
        bool compileFailed = false;

        glslang::TProgram& program = *new glslang::TProgram;
        // Build per-stage preambles (must outlive parse calls)
        std::vector<std::string> preambles;
        for (const auto& cu : compUnits) {
            std::string p = defines;
            if (cu.stage == EShLangVertex)
                p += "#define VERTEX_SHADER 1\n";
            else if (cu.stage == EShLangFragment)
                p += "#define FRAGMENT_SHADER 1\n";
            preambles.push_back(p);
        }

        static TBuiltInResource defaultBuiltInResources = InitResources();

        // SPIR-V requires modern GLSL; default to 430 (desktop profile) to support
        // layout qualifiers, in/out varyings, etc. Non-SPIR-V targets keep ESSL 100.
        const int defaultVersion = (target.lang == SpirV) ? 430 : 100;
        EProfile profile = (target.lang == SpirV) ? ECoreProfile : EEsProfile;

        int unitIdx = 0;
        for (auto it = compUnits.cbegin(); it != compUnits.cend(); ++it, ++unitIdx) {
            const auto& compUnit = *it;
            auto configure = [&](glslang::TShader& shader) {
                shader.setStringsWithLengthsAndNames(compUnit.text, NULL, compUnit.fileNameList, 1);
                shader.setPreamble(preambles[unitIdx].c_str());
                shader.setAutoMapBindings(true);
                shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

                // For SPIR-V: target Vulkan so glslang uses OriginUpperLeft for fragment shaders
                if (target.lang == SpirV)
                {
                    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
#ifdef SHADERCROSS_BGFX_VULKAN
                    useBgfxUniformBlock(shader, compUnit.stage);
                    shader.setAutoMapLocations(true);
#endif
                }
            };

#ifdef SHADERCROSS_BGFX_VULKAN
            if (target.lang == SpirV)
            {
                glslang::TShader preprocessor(compUnit.stage);
                configure(preprocessor);
                std::string flattened;
                if (preprocessor.preprocess(&defaultBuiltInResources, defaultVersion, profile, false, false, messages, &flattened, includer))
                    preambles[unitIdx] += undeclaredBuiltins(flattened);
            }
#endif

            glslang::TShader* shader = new glslang::TShader(compUnit.stage);
            configure(*shader);
            shaders.push_back(shader);

            if (!shader->parse(&defaultBuiltInResources, defaultVersion, profile, false, false, messages, includer))
            {
                compileFailed = true;
                result.errors += shader->getInfoLog();
            }

            program.addShader(shader);
        }

        //
        // Program-level processing...
        //

        // Link
        if (!program.link(messages))
        {
            linkFailed = true;
            result.errors += program.getInfoLog();
        }
            
        // Other targets retain their existing descriptor allocation. Metal's
        // texture argument indices must not count scalar/vector/matrix uniforms.
        glslang::TIntermediate* firstStage = nullptr;
        for (int stage = 0; stage < EShLangCount && !firstStage; ++stage)
            firstStage = program.getIntermediate(static_cast<EShLanguage>(stage));
        std::unique_ptr<MetalBindingResolver> metalBindings;
        if (target.lang == Metal && firstStage)
        {
            metalBindings = std::make_unique<MetalBindingResolver>(*firstStage);
            // Collect every stage before allocation so explicit fragment slots
            // are reserved before implicit vertex textures. Shared resources use
            // one slot: bgfx's texture table is shared across shader stages.
            for (int stage = 0; stage < EShLangCount; ++stage)
                if (auto intermediate = program.getIntermediate(static_cast<EShLanguage>(stage)))
                    metalBindings->reserveExplicitBindings(*intermediate);
        }
        if (!program.mapIO(metalBindings.get()))
        {
            linkFailed = true;
        }
        
        // Dump SPIR-V
        if (compileFailed || linkFailed)
        {
            result.success = false;
            result.errors += "SPIR-V is not generated for failed compile or link\n";
        }
        else
        {
            int outputIndex = 0;
            for (int stage = 0; stage < EShLangCount; ++stage)
            {
                if (program.getIntermediate((EShLanguage)stage))
                {
                    std::vector<unsigned int> spirv;
                    std::string warningsErrors;
                    spv::SpvBuildLogger logger;
                    glslang::GlslangToSpv(*program.getIntermediate((EShLanguage)stage), spirv, &logger);

                    if (stage == EShLangVertex)
                    {
                        std::string inputError;
                        if (!assignVertexInputNames(spirv, inputError))
                        {
                            compileFailed = true;
                            result.success = false;
                            result.errors += inputError + "\n";
                            continue;
                        }
                    }

#ifdef SHADERCROSS_BGFX_VULKAN
                    if (target.lang == SpirV)
                    {
                        std::string bindingError;
                        if (!assignBgfxBindings(spirv, static_cast<EShLanguage>(stage), &bindingError))
                        {
                            compileFailed = true;
                            result.success = false;
                            result.errors += bindingError + "\n";
                            continue;
                        }
                    }
#endif

                    Translator* translator = NULL;
                    ShaderStage shaderStage = shLanguageToShaderStage((EShLanguage)stage);
                    std::map<std::string, int> attributes;
                    
                    switch (target.lang)
                    {
                    case SpirV:
                        // Bypass SpirVTranslator — it produces invalid SPIR-V
                        // (misplaced OpSource, duplicate Location decorations).
                        // Use raw glslang SPIR-V directly; shaders must use explicit UBO blocks.
                        {
                            size_t byteSize = spirv.size() * sizeof(unsigned int);
                            result.output[outputIndex] = std::string(reinterpret_cast<const char*>(spirv.data()), byteSize);
                            result.success = !compileFailed;
                            result.resultCount = 1;
                        }
                        break;
                    case GLSL:
                        translator = new GlslTranslator2(spirv, shaderStage, false);
                        break;
                    case HLSL:
                        translator = new HlslTranslator2(spirv, shaderStage);
                        break;
                    case Metal:
                        translator = new MetalTranslator2(spirv, shaderStage);
                        break;
                    case AGAL:
                        translator = new AgalTranslator(spirv, shaderStage);
                        break;
                    case VarList:
                        translator = new VarListTranslator(spirv, shaderStage);
                        break;
                    case JavaScript:
                        break;
                    }

                    if (translator)
                    {
                        try
                        {
                            translator->outputCode(target, sourcefilename, filename, compilerOutputBuffer.data(), attributes);
                            result.output[outputIndex] = compilerOutputBuffer.data();
                            result.success = !compileFailed;
                            result.resultCount = 1;
                        }
                        catch (spirv_cross::CompilerError& error) {
                            printf("Error compiling to %s: %s\n", target.string().c_str(), error.what());
                            compileFailed = true;
                            result.success = false;
                            result.errors += error.what();
                        }
                    }
                    
                    {
                        spirv_cross::Parser spirv_parser(spirv);
                        spirv_parser.parse();

                        spirv_cross::CompilerReflection compiler(std::move(spirv_parser.get_parsed_ir()));
                        compiler.set_format("json");
                        
                        result.json[outputIndex] = compiler.compile();
                    }

                    outputIndex++;
                    
                    delete translator;
                }
            }
        }
        
        // Free everything up, program has to go before the shaders
        // because it might have merged stuff from the shaders, and
        // the stuff from the shaders has to have its destructors called
        // before the pools holding the memory in the shaders is freed.
        delete& program;
        while (shaders.size() > 0)
        {
            delete shaders.back();
            shaders.pop_back();
        }
    }

    void CompileAndLinkShaderFiles(const Config& config,
                                   Result& result,
                                   Target target,
                                   glslang::TShader::Includer& includer,
                                   const char* defines)
    {
        std::vector<ShaderCompUnit> compUnits;

        char* sources[] = { nullptr, nullptr, nullptr, nullptr, nullptr };

        for (int i = 0; i < config.stageCount; i++)
        {
            sources[i] = (char*)config.source[i].c_str();
            
            const char* to;
            EShLanguage lang = EShLangCount;
            
            switch(config.stage[i])
            {
                case StageVertex:
                    to = "vert";
                    lang = EShLangVertex;
                    break;
                case StageTessControl:
                    to = "tesc";
                    lang = EShLangTessControl;
                    break;
                case StageTessEvaluation:
                    to = "tese";
                    lang = EShLangTessEvaluation;
                    break;
                case StageGeometry:
                    to = "geom";
                    lang = EShLangGeometry;
                    break;
                case StageFragment:
                    to = "frag";
                    lang = EShLangFragment;
                    break;
                case StageCompute:
                    to = "comp";
                    lang = EShLangCompute;
                    break;
                default:
                    break;
            }
            
            std::string name = (config.sourceName[i].length() > 0) ? config.sourceName[i] : std::string("source.") + to;
                        
            ShaderCompUnit compUnit(lang, name, sources+i);
            compUnits.push_back(compUnit);
        }
    
        result.success = true;
        
        CompileAndLinkShaderUnits(config,
                                  result,
                                  compUnits,
                                  target,
                                  config.sourceName[0].c_str(),
                                  config.sourceName[0].c_str(),
                                  includer,
                                  defines);

    }

    void Compile(const Config& config, Result& result)
    {
        glslang::TShader::Includer* includer = nullptr;
        
        if (config.includeCallback)
        {
            includer = new CustomIncluder(*config.includeCallback);
        }
        else if (config.includePath.length() > 0)
        {
            includer = new KrafixIncluder(config.includePath);
        }
        else
        {
            includer = new NullIncluder();
        }
                    
        static std::once_flag initializeProcess;
        std::call_once(initializeProcess, [] { glslang::InitializeProcess(); });

        int version = -1;

        Target target = config.target;
        std::string defines = config.defines;

        switch(config.target.lang)
        {
            case SpirV:
                target.version = version > 0 ? version : 1;
                defines += "#extension GL_GOOGLE_include_directive : enable\n";
                defines += "#define SPIRV " + std::to_string(config.target.version) + "\n";
                defines += "#define TARGET_LANGUAGE_SPIRV 1\n";
                break;
            case GLSL:
                defines += "#define GLSL " + std::to_string(target.version) + "\n";
                break;
            case HLSL:
                target.version = version > 0 ? version : 11;
                defines += "#define HLSL " + std::to_string(target.version) + "\n";
                break;
            case Metal:
                target.version = version > 0 ? version : 1;
                defines += "#define METAL " + std::to_string(target.version) + "\n";
                break;
            case AGAL:
                target.version = version > 0 ? version : 100;
                target.es = true;
                defines += "#define AGAL " + std::to_string(target.version) + "\n";
                break;
            case VarList:
                target.version = version > 0 ? version : 1;
                break;
            case JavaScript:
                result.success = false;
                result.errors = "JavaScript not supported";
                return;
        }
        
        CompileAndLinkShaderFiles(config,
                                  result,
                                  target,
                                  *includer,
                                  defines.c_str());

        //glslang::FinalizeProcess();

        if (includer) delete includer;
    }
}


