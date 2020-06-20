//
//  Common.h
//  ShaderCross
//
//  Created by John Millard on 14/6/20.
//  Copyright © 2020 John Millard. All rights reserved.
//

#ifndef Common_h
#define Common_h

#include <map>
#include <string>
#include <sstream>
#include <vector>

namespace krafix {
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
}

#endif /* Common_h */
