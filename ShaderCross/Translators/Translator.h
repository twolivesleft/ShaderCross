#pragma once

#include "ShaderCross.hpp"

#include <SPIRV-Cross/spirv.hpp>

namespace ShaderCross {

	class Instruction {
	public:
		Instruction(std::vector<unsigned>& spirv, unsigned& index);
		Instruction(int opcode, unsigned* operands, unsigned length);

		int opcode;
		unsigned* operands;
		unsigned length;
		const char* string;
	};

	class Translator {
	public:
		Translator(std::vector<unsigned>& spirv, ShaderStage stage);
		virtual ~Translator() {}
		virtual void outputCode(const Target& target, const char* sourcefilename, const char* filename, char* output, std::map<std::string, int>& attributes) = 0;

	protected:
		std::vector<unsigned>& spirv;
		std::vector<Instruction> instructions;
		ShaderStage stage;
		spv::ExecutionModel executionModel();

		unsigned magicNumber;
		unsigned version;
		unsigned generator;
		unsigned bound;
		unsigned schema;
	};
}
