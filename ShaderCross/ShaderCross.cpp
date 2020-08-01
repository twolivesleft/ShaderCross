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

    // TODO: Find out what the hell this thing does
    static void preprocessSpirv(std::vector<unsigned int>& spirv)
    {
        unsigned binding = 0;
        for (unsigned index = 0; index < spirv.size(); ++index)
        {
            int wordCount = spirv[index] >> 16;
            int opcode = spirv[index] & 0xffff;

            unsigned* operands = wordCount > 1 ? &spirv[index + 1] : NULL;
            int length = wordCount - 1;

            if (opcode == 71 && length >= 2)
            {
                if (operands[1] == 33)
                {
                    operands[2] = binding++;
                }
            }
        }
    }

    static char s_compilerOutputBuffer[1024*1024];

    //
    //   Deduce the language from the filename.  Files must end in one of the
    //   following extensions:
    //
    //   .vert = vertex
    //   .tesc = tessellation control
    //   .tese = tessellation evaluation
    //   .geom = geometry
    //   .frag = fragment
    //   .comp = compute
    //
    EShLanguage FindLanguage(const std::string& name, bool parseSuffix)
    {
        size_t ext = 0;
        std::string suffix;

        // Search for a suffix on a filename: e.g, "myfile.frag".  If given
        // the suffix directly, we skip looking for the '.'
        if (parseSuffix) {
            ext = name.rfind('.');
            if (ext == std::string::npos) {
                return EShLangVertex;
            }
            ++ext;
        }
        suffix = name.substr(ext, std::string::npos);

        if (suffix == "glsl") {
            size_t ext2 = name.substr(0, ext - 1).rfind('.');
            suffix = name.substr(ext2 + 1, ext - ext2 - 2);
        }

        if (suffix == "vert")
            return EShLangVertex;
        else if (suffix == "tesc")
            return EShLangTessControl;
        else if (suffix == "tese")
            return EShLangTessEvaluation;
        else if (suffix == "geom")
            return EShLangGeometry;
        else if (suffix == "frag")
            return EShLangFragment;
        else if (suffix == "comp")
            return EShLangCompute;
        
        return EShLangVertex;
    }

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
                                   const char* tempdir,
                                   glslang::TShader::Includer& includer,
                                   const char* defines)
    {
        // keep track of what to free
        std::list<glslang::TShader*> shaders;

        EShMessages messages = EShMsgDefault;
//        SetMessageOptions(messages);

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
//            if (entryPointName) // HLSL todo: this needs to be tracked per compUnits
//                shader->setEntryPoint(entryPointName);
//            if (sourceEntryPointName)
//                shader->setSourceEntryPoint(sourceEntryPointName);

//            shader->setShiftSamplerBinding(baseSamplerBinding[compUnit.stage]);
//            shader->setShiftTextureBinding(baseTextureBinding[compUnit.stage]);
//            shader->setShiftImageBinding(baseImageBinding[compUnit.stage]);
//            shader->setShiftUboBinding(baseUboBinding[compUnit.stage]);
//            shader->setShiftSsboBinding(baseSsboBinding[compUnit.stage]);
//            shader->setFlattenUniformArrays((Options & EOptionFlattenUniformArrays) != 0);
//            shader->setNoStorageFormat((Options & EOptionNoStorageFormat) != 0);
            shader->setPreamble(defines);

//            if (Options & EOptionAutoMapBindings)
            shader->setAutoMapBindings(true);

            shaders.push_back(shader);

            const int defaultVersion = 100; // Options & EOptionDefaultDesktop ? 110 : 100;

//            if (Options & EOptionOutputPreprocessed) {
//                std::string str;
//                //glslang::TShader::ForbidIncluder includer;
//                if (shader->preprocess(&Resources, defaultVersion, ENoProfile, false, false,
//                    messages, &str, includer)) {
//                    PutsIfNonEmpty(str.c_str());
//                }
//                else {
//                    CompileFailed = true;
//                }
//                StderrIfNonEmpty(shader->getInfoLog());
//                StderrIfNonEmpty(shader->getInfoDebugLog());
//                continue;
//            }
            
//            shader->setEnvInput((Options & EOptionReadHlsl) ? glslang::EShSourceHlsl
//                                                            :
//                                compUnit.stage, Client, ClientInputSemanticsVersion);
//            shader->setEnvClient(Client, ClientVersion);
            shader->setEnvTarget(glslang::EshTargetSpv, glslang::EShTargetSpv_1_0);

            
            if (!shader->parse(&glslang::DefaultTBuiltInResource, defaultVersion, ENoProfile, false, false, messages, includer))
            {
                compileFailed = true;
                result.errors += shader->getInfoLog();
            }

            program.addShader(shader);

//            if (!(Options & EOptionSuppressInfolog) &&
//                !(Options & EOptionMemoryLeakMode)) {
//                //PutsIfNonEmpty(compUnit.fileName.c_str());
//                PutsIfNonEmpty(shader->getInfoLog());
//                PutsIfNonEmpty(shader->getInfoDebugLog());
//            }
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
        
//        // Report
//        if (!(Options & EOptionSuppressInfolog) &&
//            !(Options & EOptionMemoryLeakMode)) {
//            PutsIfNonEmpty(program.getInfoLog());
//            PutsIfNonEmpty(program.getInfoDebugLog());
//        }

        // Reflect
//        if (Options & EOptionDumpReflection) {
//            program.buildReflection();
//            program.dumpReflection();
//        }

        // Dump SPIR-V
        if (compileFailed || linkFailed)
        {
            result.success = false;
            result.errors += "SPIR-V is not generated for failed compile or link\n";
        }
        else
        {
            for (int stage = 0; stage < EShLangCount; ++stage) {
                if (program.getIntermediate((EShLanguage)stage)) {
                    std::vector<unsigned int> spirv;
                    std::string warningsErrors;
                    spv::SpvBuildLogger logger;
                    glslang::GlslangToSpv(*program.getIntermediate((EShLanguage)stage), spirv, &logger);

//                    preprocessSpirv(spirv);

                    Translator* translator = NULL;
                    ShaderStage shaderStage = shLanguageToShaderStage((EShLanguage)stage);
                    std::map<std::string, int> attributes;
                    
                    
                    switch (target.lang) {
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

                    try {
                        if (target.lang == HLSL && target.system != Unity) {
                            // TODO: put this back in
//                            std::string temp = sourcefilename == nullptr ? "" : std::string(tempdir) + "/" + removeExtension(extractFilename(sourcefilename)) + ".hlsl";
//                            char* tempoutput = nullptr;
//                            if (output) {
//                                tempoutput = new char[1024 * 1024];
//                            }
//                            translator->outputCode(target, sourcefilename, temp.c_str(), tempoutput, attributes);
//                            int returnCode = 0;
//                            if (target.version == 9) {
//                                returnCode = compileHLSLToD3D9(temp.c_str(), filename, tempoutput, output, length, attributes, (EShLanguage)stage);
//                            }
//                            else {
//                                returnCode = compileHLSLToD3D11(temp.c_str(), filename, tempoutput, output, length, attributes, (EShLanguage)stage, debugMode);
//                            }
//                            if (returnCode != 0) CompileFailed = true;
//                            delete[] tempoutput;
                        }
                        else {
                            translator->outputCode(target, sourcefilename, filename, s_compilerOutputBuffer, attributes);
                            result.output = s_compilerOutputBuffer;
                            result.success = true;
                        }
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
                        
                        result.json = compiler.compile();
                    }

                    delete translator;

                    //glslang::OutputSpv(spirv, GetBinaryName((EShLanguage)stage));
//                    if (Options & EOptionHumanReadableSpv) {
//                        spv::Disassemble(std::cout, spirv);
//                    }
                }
            }
        }
        
        // Free everything up, program has to go before the shaders
        // because it might have merged stuff from the shaders, and
        // the stuff from the shaders has to have its destructors called
        // before the pools holding the memory in the shaders is freed.
        delete& program;
        while (shaders.size() > 0) {
            delete shaders.back();
            shaders.pop_back();
        }
    }

    void CompileAndLinkShaderFiles(const Config& config,
                                   Result& result,
                                   glslang::TWorklist& workList,
                                   Target target,
                                   const char* sourcefilename,
                                   const char* filename,
                                   const char* tempdir,
                                   glslang::TShader::Includer& includer,
                                   const char* defines)
    {
        std::vector<ShaderCompUnit> compUnits;

        // Transfer all the work items from to a simple list of
        // of compilation units.  (We don't care about the thread
        // work-item distribution properties in this path, which
        // is okay due to the limited number of shaders, know since
        // they are all getting linked together.)

        char* sources[] = { (char*)config.source.c_str(), nullptr, nullptr, nullptr, nullptr };

        glslang::TWorkItem* workItem;
        while (workList.remove(workItem)) {
            ShaderCompUnit compUnit(
                FindLanguage(workItem->name, true),
                workItem->name,
                sources
            );

            if (!compUnit.text) {
                return;
            }

            compUnits.push_back(compUnit);
        }

        result.success = true;
        
        // Actual call to programmatic processing of compile and link,
        // in a loop for testing memory and performance.  This part contains
        // all the perf/memory that a programmatic consumer will care about.
        for (int i = 0; i < 1; ++i)
        {
            for (int j = 0; j < 1; ++j)
            {
                CompileAndLinkShaderUnits(config,
                                          result,
                                          compUnits,
                                          target,
                                          sourcefilename,
                                          filename, tempdir,
                                          includer,
                                          defines);
            }

//            if (Options & EOptionMemoryLeakMode)
//                glslang::OS_DumpMemoryCounters();
        }

//        if (source == nullptr) {
//            for (auto it = compUnits.begin(); it != compUnits.end(); ++it)
//                FreeFileData(it->text);
//        }
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
        
        glslang::TWorklist Worklist;

        // array of unique places to leave the shader names and infologs for the asynchronous compiles
        glslang::TWorkItem** Work = 0;
        int NumWorkItems = 0;

        NumWorkItems = 1;
        Work = new glslang::TWorkItem * [NumWorkItems];
        Work[0] = 0;

        std::string to;
        switch(config.stage)
        {
            case StageVertex:
                to = "vert";
                break;
            case StageTessControl:
                to = "tesc";
                break;
            case StageTessEvaluation:
                to = "tese";
                break;
            case StageGeometry:
                to = "geom";
                break;
            case StageFragment:
                to = "frag";
                break;
            case StageCompute:
                to = "comp";
                break;
        }
        
        if (config.sourceName.length() > 0)
        {
            std::string name(config.sourceName);
            Work[0] = new glslang::TWorkItem(name);
            Worklist.add(Work[0]);
        }
        else
        {
            std::string name = std::string("source.") + to;
            Work[0] = new glslang::TWorkItem(name);
            Worklist.add(Work[0]);
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
                                  Worklist,
                                  target,
                                  config.sourceName.c_str(),
                                  to.c_str(),
                                  nullptr,
                                  *includer,
                                  defines.c_str());

        glslang::FinalizeProcess();

        if (includer) delete includer;
    }
}


