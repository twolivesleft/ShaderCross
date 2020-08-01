//
//  ShaderCross.hpp
//  ShaderCross
//
//  Created by John Millard on 18/6/20.
//  Copyright © 2020 John Millard. All rights reserved.
//

#ifndef ShaderCross_hpp
#define ShaderCross_hpp

#include <map>
#include <string>
#include <sstream>
#include <vector>

namespace ShaderCross
{
    typedef std::pair<std::string, std::string> IncludeCallbackResult;
    typedef std::function<IncludeCallbackResult(const char* headerName, bool local)> IncludeCallback;

    enum TargetLanguage {
        SpirV,
        GLSL,
        HLSL,
        Metal,
        AGAL,
        VarList,
        JavaScript
    };

    enum ShaderStage {
        StageVertex,
        StageTessControl,
        StageTessEvaluation,
        StageGeometry,
        StageFragment,
        StageCompute
    };

    enum TargetSystem {
        Windows,
        WindowsApp,
        OSX,
        Linux,
        iOS,
        Android,
        HTML5,
        Flash,
        Unity,
        Unknown
    };

    struct Target {
        TargetLanguage lang;
        int version;
        bool es;
        TargetSystem system;

        std::string string() {
            switch (lang) {
            case SpirV:
                return "SPIR-V";
            case GLSL: {
                std::stringstream stream;
                stream << "GLSL ";
                if (es) stream << "ES ";
                stream << version;
                return stream.str();
            }
            case HLSL: {
                std::stringstream stream;
                stream << "HLSL ";
                stream << version;
                return stream.str();
            }

            case Metal:
                return "Metal";
            case AGAL:
                return "AGAL";
            case VarList:
                return "VarList";
            case JavaScript:
                return "JavaScript";
            }
            return "Unknown";
        }
    };

    struct Config
    {
        Target target;
        ShaderStage stage;
        std::string source;
        std::string sourceName;
        std::string defines;
        std::string includePath;
        IncludeCallback* includeCallback;
    };

    struct Result
    {
        bool success; /* success/failure result of compilation */
        std::string output; /* cross-compiled source code */
        std::string errors; /* compiler and linker errors */
        std::string json; /* reflection data */
    };

    void Compile(const Config& config, Result& result);
}

#endif /* ShaderCross_hpp */
