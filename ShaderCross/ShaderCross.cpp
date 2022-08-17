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

#include <glslang/StandAlone/ResourceLimits.h>
#include <glslang/StandAlone/Worklist.h>
#include <glslang/glslang/Include/ShHandle.h>
#include <glslang/glslang/Include/revision.h>
#include <glslang/glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/SPIRV/GLSL.std.450.h>
#include <glslang/SPIRV/doc.h>
#include <glslang/SPIRV/disassemble.h>
#include <glslang/OSDependent/osinclude.h>

#include <SPIRV-Cross/spirv_parser.hpp>
#include <SPIRV-Cross/spirv_reflect.hpp>
#include <SPIRV-Cross/spirv_common.hpp>

#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <array>
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

    static char s_compilerOutputBuffer[1024*1024];

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
        // keep track of what to free
        std::list<glslang::TShader*> shaders;

        EShMessages messages = EShMsgDefault;

        //
        // Per-shader processing...
        //
        
        bool linkFailed = false;
        bool compileFailed = false;

        glslang::TProgram& program = *new glslang::TProgram;
        for (auto it = compUnits.cbegin(); it != compUnits.cend(); ++it) {
            const auto& compUnit = *it;
            glslang::TShader* shader = new glslang::TShader(compUnit.stage);
            shader->setStringsWithLengthsAndNames(compUnit.text, NULL, compUnit.fileNameList, 1);
            shader->setPreamble(defines);
            shader->setAutoMapBindings(true);

            shaders.push_back(shader);

            const int defaultVersion = 100; // Options & EOptionDefaultDesktop ? 110 : 100;

            shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
            
            static TBuiltInResource defaultBuiltInResources = InitResources();
            
            if (!shader->parse(&defaultBuiltInResources, defaultVersion, EEsProfile, false, false, messages, includer))
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
            
        if (!program.mapIO())
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

                    Translator* translator = NULL;
                    ShaderStage shaderStage = shLanguageToShaderStage((EShLanguage)stage);
                    std::map<std::string, int> attributes;
                    
                    switch (target.lang)
                    {
                    case SpirV:
                        translator = new SpirVTranslator(spirv, shaderStage);
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

                    try
                    {
                        translator->outputCode(target, sourcefilename, filename, s_compilerOutputBuffer, attributes);
                        result.output[outputIndex] = s_compilerOutputBuffer;
                        result.success = true;
                        result.resultCount = 1;
                    }
                    catch (spirv_cross::CompilerError& error) {
                        printf("Error compiling to %s: %s\n", target.string().c_str(), error.what());
                        compileFailed = true;
                        result.success = false;
                        result.errors += error.what();
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
                    
        glslang::InitializeProcess();

        int version = -1;

        Target target = config.target;
        std::string defines = config.defines;

        switch(config.target.lang)
        {
            case SpirV:
                target.version = version > 0 ? version : 1;
                defines += "#define SPIRV " + std::to_string(config.target.version) + "\n";
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


