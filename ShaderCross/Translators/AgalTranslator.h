#pragma once

#include "Translator.h"
#include <glslang/SPIRV/GLSL.std.450.h>

namespace ShaderCross {
	class AgalTranslator : public Translator {
	public:
		AgalTranslator(std::vector<unsigned>& spirv, ShaderStage stage) : Translator(spirv, stage) {}
		void outputCode(const Target& target, const char* sourcefilename, const char* filename, char* output, std::map<std::string, int>& attributes) override;
	};
}
