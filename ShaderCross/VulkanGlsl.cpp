//
//  VulkanGlsl.cpp
//  ShaderCross
//

#include "VulkanGlsl.hpp"

#include <spirv-tools/optimizer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <cctype>
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
        const std::vector<std::pair<const char*, const char*>> kBuiltinUniforms =
        {
            {"u_viewRect", "vec4 u_viewRect"},
            {"u_viewTexel", "vec4 u_viewTexel"},
            {"u_view", "mat4 u_view"},
            {"u_invView", "mat4 u_invView"},
            {"u_proj", "mat4 u_proj"},
            {"u_invProj", "mat4 u_invProj"},
            {"u_viewProj", "mat4 u_viewProj"},
            {"u_invViewProj", "mat4 u_invViewProj"},
            {"u_model", "mat4 u_model[128]"},
            {"u_modelView", "mat4 u_modelView"},
            {"u_modelViewProj", "mat4 u_modelViewProj"},
            {"u_alphaRef4", "vec4 u_alphaRef4"},
            {"u_normal", "mat3 u_normal"},
            {"u_cameraPosition", "vec3 u_cameraPosition"},
            {"u_time", "vec4 u_time"},
            {"u_tintColor", "vec4 u_tintColor"},
        };
        const std::vector<const char*> kBuiltinSamplers = {"s_texture", "s_curveAtlasTexture"};

        struct Token
        {
            std::string text;
            size_t pos;
        };

        bool isIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        // The source with comments and preprocessor lines blanked out, newlines kept, so offsets match
        std::string maskNonCode(const std::string& s)
        {
            std::string m = s;
            bool lineStart = true;
            for (size_t i = 0; i < m.size(); )
            {
                char c = m[i];
                if (c == '\n') { lineStart = true; i++; continue; }
                if (lineStart && (c == ' ' || c == '\t' || c == '\r')) { i++; continue; }
                if (lineStart && c == '#')
                {
                    while (i < m.size() && m[i] != '\n')
                    {
                        bool continued = m[i] == '\\' && i + 1 < m.size() && m[i + 1] == '\n';
                        m[i] = ' ';
                        i += continued ? 2 : 1;
                    }
                    continue;
                }
                lineStart = false;
                if (c == '/' && i + 1 < m.size() && m[i + 1] == '/')
                {
                    while (i < m.size() && m[i] != '\n') m[i++] = ' ';
                    continue;
                }
                if (c == '/' && i + 1 < m.size() && m[i + 1] == '*')
                {
                    while (i < m.size() && !(m[i] == '*' && i + 1 < m.size() && m[i + 1] == '/'))
                    {
                        if (m[i] != '\n') m[i] = ' ';
                        i++;
                    }
                    if (i < m.size()) { m[i] = ' '; m[i + 1] = ' '; i += 2; }
                    continue;
                }
                i++;
            }
            return m;
        }

        std::vector<Token> tokenize(const std::string& m, size_t begin, size_t end)
        {
            std::vector<Token> tokens;
            for (size_t i = begin; i < end; )
            {
                char c = m[i];
                if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }
                if (isIdentifierChar(c))
                {
                    // Numbers keep their decimal point; identifiers stop at member access
                    bool number = std::isdigit(static_cast<unsigned char>(c));
                    size_t j = i;
                    while (j < end && (isIdentifierChar(m[j]) || (number && m[j] == '.'))) j++;
                    tokens.push_back({m.substr(i, j - i), i});
                    i = j;
                    continue;
                }
                tokens.push_back({std::string(1, c), i});
                i++;
            }
            return tokens;
        }

        size_t matchingBrace(const std::string& m, size_t open)
        {
            int depth = 0;
            for (size_t i = open; i < m.size(); i++)
            {
                if (m[i] == '{') depth++;
                else if (m[i] == '}' && --depth == 0) return i;
            }
            return m.size() - 1;
        }

        bool hasWord(const std::vector<Token>& tokens, const char* word)
        {
            return std::any_of(tokens.begin(), tokens.end(), [&](const Token& t) { return t.text == word; });
        }

        struct Range { size_t begin, end; };

        // Global-scope statements; function definitions end at their closing brace
        std::vector<Range> globalStatements(const std::string& m)
        {
            std::vector<Range> statements;
            size_t start = std::string::npos;
            for (size_t i = 0; i < m.size(); i++)
            {
                char c = m[i];
                if (start == std::string::npos)
                {
                    if (std::isspace(static_cast<unsigned char>(c))) continue;
                    start = i;
                }
                if (c == ';')
                {
                    statements.push_back({start, i + 1});
                    start = std::string::npos;
                }
                else if (c == '{')
                {
                    size_t close = matchingBrace(m, i);
                    auto head = tokenize(m, start, i);
                    bool declaration = hasWord(head, "struct") || hasWord(head, "uniform") || hasWord(head, "buffer");
                    i = close;
                    if (!declaration)
                    {
                        statements.push_back({start, close + 1});
                        start = std::string::npos;
                    }
                }
            }
            return statements;
        }

        int layoutBinding(const std::vector<Token>& tokens)
        {
            for (size_t i = 0; i + 2 < tokens.size(); i++)
            {
                if (tokens[i].text == "binding" && tokens[i + 1].text == "=") return std::atoi(tokens[i + 2].text.c_str());
            }
            return -1;
        }

        bool isQualifier(const std::string& word)
        {
            static const std::set<std::string> qualifiers = {
                "uniform", "highp", "mediump", "lowp", "const", "flat", "smooth", "noperspective",
                "readonly", "writeonly", "coherent", "volatile", "restrict", "precise", "invariant"};
            return qualifiers.count(word) > 0;
        }

        bool isOpaque(const std::string& type)
        {
            for (const char* prefix : {"sampler", "isampler", "usampler", "texture", "itexture", "utexture",
                                       "image", "iimage", "uimage", "subpassInput", "atomic_uint"})
            {
                if (type.rfind(prefix, 0) == 0) return true;
            }
            return false;
        }

        struct Variable { std::string name, array; };

        struct UniformDeclaration
        {
            Range range;
            std::string type;
            std::vector<Variable> variables;
            int binding = -1;
            bool isBlock = false;
            bool hasInstanceName = false;
            std::string members;          // for blocks: the text between the braces
        };

        bool parseUniform(const std::string& s, const std::string& m, Range range, UniformDeclaration& out)
        {
            auto tokens = tokenize(m, range.begin, range.end);
            if (!hasWord(tokens, "uniform")) return false;

            out.range = range;
            out.binding = layoutBinding(tokens);

            auto open = std::find_if(tokens.begin(), tokens.end(), [](const Token& t) { return t.text == "{"; });
            if (open != tokens.end())
            {
                size_t close = matchingBrace(m, open->pos);
                out.isBlock = true;
                out.members = s.substr(open->pos + 1, close - open->pos - 1);
                auto tail = tokenize(m, close + 1, range.end);
                out.hasInstanceName = !tail.empty() && isIdentifierChar(tail.front().text[0]);
                return true;
            }

            // Skip layout(...) and qualifiers to reach the type, then read the declared names
            size_t i = 0;
            if (i < tokens.size() && tokens[i].text == "layout")
            {
                int depth = 0;
                for (; i < tokens.size(); i++)
                {
                    if (tokens[i].text == "(") depth++;
                    else if (tokens[i].text == ")" && --depth == 0) { i++; break; }
                }
            }
            while (i < tokens.size() && isQualifier(tokens[i].text)) i++;
            if (i >= tokens.size()) return false;
            out.type = tokens[i++].text;

            while (i < tokens.size() && tokens[i].text != ";")
            {
                if (!isIdentifierChar(tokens[i].text[0])) { i++; continue; }
                Variable variable{tokens[i++].text, ""};
                if (i < tokens.size() && tokens[i].text == "[")
                {
                    size_t close = i;
                    while (close < tokens.size() && tokens[close].text != "]") close++;
                    if (close < tokens.size())
                    {
                        variable.array = m.substr(tokens[i].pos, tokens[close].pos - tokens[i].pos + 1);
                        i = close + 1;
                    }
                }
                out.variables.push_back(variable);
                while (i < tokens.size() && tokens[i].text != "," && tokens[i].text != ";") i++;
                if (i < tokens.size() && tokens[i].text == ",") i++;
            }
            return !out.variables.empty();
        }

        std::set<std::string> memberNames(const std::string& members)
        {
            std::set<std::string> names;
            std::string masked = maskNonCode(members);
            size_t start = 0;
            for (size_t i = 0; i < masked.size(); i++)
            {
                if (masked[i] != ';') continue;
                auto tokens = tokenize(masked, start, i);
                for (size_t t = tokens.size(); t-- > 0; )
                {
                    if (tokens[t].text == "]")
                    {
                        while (t > 0 && tokens[t].text != "[") t--;
                        continue;
                    }
                    if (isIdentifierChar(tokens[t].text[0])) { names.insert(tokens[t].text); break; }
                }
                start = i + 1;
            }
            return names;
        }
    }

    namespace
    {
        // What one stage declares, and what its uniform block needs
        struct StageUniforms
        {
            std::vector<Range> statements;
            std::vector<UniformDeclaration> loose;
            std::vector<UniformDeclaration> anonymousBlocks;
            std::vector<std::string> members;
            std::vector<std::string> builtinSamplers;
            bool needsBlock = false;
        };

        // The member declarations of a block's body, one per entry
        std::vector<std::string> splitMembers(const std::string& body)
        {
            std::vector<std::string> members;
            std::string masked = maskNonCode(body);
            size_t start = 0;
            for (size_t i = 0; i < masked.size(); i++)
            {
                if (masked[i] != ';') continue;
                std::string member = masked.substr(start, i - start + 1);
                member.erase(0, member.find_first_not_of(" \t\r\n"));
                if (member.size() > 1) members.push_back(member);
                start = i + 1;
            }
            return members;
        }

        std::string memberName(const std::string& member)
        {
            auto names = memberNames(member);
            return names.empty() ? member : *names.begin();
        }

        StageUniforms analyze(const std::string& source, const std::string& masked)
        {
            StageUniforms stage;
            stage.statements = globalStatements(masked);

            std::set<std::string> declared;
            for (const Range& range : stage.statements)
            {
                UniformDeclaration declaration;
                if (!parseUniform(source, masked, range, declaration)) continue;

                if (declaration.isBlock)
                {
                    auto names = memberNames(declaration.members);
                    declared.insert(names.begin(), names.end());
                    if (!declaration.hasInstanceName) stage.anonymousBlocks.push_back(declaration);
                    continue;
                }

                for (const Variable& variable : declaration.variables) declared.insert(variable.name);
                if (!isOpaque(declaration.type)) stage.loose.push_back(declaration);
            }

            // bgfx built-ins the shader uses without declaring
            std::set<std::string> used;
            for (const Token& token : tokenize(masked, 0, masked.size()))
            {
                if (isIdentifierChar(token.text[0])) used.insert(token.text);
            }
            std::vector<std::string> builtins;
            for (const auto& [name, declaration] : kBuiltinUniforms)
            {
                if (used.count(name) && !declared.count(name)) builtins.push_back(std::string(declaration) + ";");
            }
            for (const char* name : kBuiltinSamplers)
            {
                if (used.count(name) && !declared.count(name)) stage.builtinSamplers.push_back(name);
            }

            stage.needsBlock = !stage.loose.empty() || !builtins.empty();
            for (const UniformDeclaration& block : stage.anonymousBlocks)
            {
                for (const std::string& member : splitMembers(block.members)) stage.members.push_back(member);
            }
            stage.members.insert(stage.members.end(), builtins.begin(), builtins.end());
            for (const UniformDeclaration& declaration : stage.loose)
            {
                for (const Variable& variable : declaration.variables)
                {
                    stage.members.push_back(declaration.type + " " + variable.name + variable.array + ";");
                }
            }
            return stage;
        }

        std::string rewrite(const std::string& source, const StageUniforms& stage, const std::vector<std::string>& members)
        {
            struct Edit { size_t begin, end; std::string text; };
            std::vector<Edit> edits;
            size_t insertAt = stage.statements.front().begin;

            if (!members.empty())
            {
                // Ahead of every use: the first statement, as the members only use built-in types
                std::string body;
                std::set<std::string> seen;
                for (const std::string& member : members)
                {
                    if (seen.insert(memberName(member)).second) body += "\n    " + member;
                }
                for (const UniformDeclaration& declaration : stage.loose) edits.push_back({declaration.range.begin, declaration.range.end, ""});
                for (const UniformDeclaration& block : stage.anonymousBlocks) edits.push_back({block.range.begin, block.range.end, ""});
                edits.push_back({insertAt, insertAt, "layout(std140, binding = 0) uniform CodeaUniforms {" + body + "\n};\n"});
            }

            if (!stage.builtinSamplers.empty())
            {
                std::string declarations = "\n";
                for (const std::string& name : stage.builtinSamplers) declarations += "uniform sampler2D " + name + ";\n";
                edits.push_back({insertAt, insertAt, declarations});
            }

            // Apply from the end so earlier offsets stay valid. At one offset, removals go before
            // insertions, which would otherwise be removed with them
            std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b)
            {
                return a.begin != b.begin ? a.begin > b.begin : a.end > b.end;
            });
            std::string result = source;
            for (const Edit& edit : edits) result.replace(edit.begin, edit.end - edit.begin, edit.text);
            return result;
        }
    }

    std::vector<std::string> adaptGlslForVulkan(const std::vector<std::string>& stages)
    {
        std::vector<std::string> masked;
        std::vector<StageUniforms> analyzed;
        for (const std::string& source : stages)
        {
            masked.push_back(maskNonCode(source));
            analyzed.push_back(analyze(source, masked.back()));
        }

        // Stages linked into one program share a block, since anonymous members are global names
        bool needsBlock = false;
        std::vector<std::string> members;
        for (const StageUniforms& stage : analyzed)
        {
            needsBlock |= stage.needsBlock;
            members.insert(members.end(), stage.members.begin(), stage.members.end());
        }

        std::vector<std::string> result;
        for (size_t i = 0; i < stages.size(); i++)
        {
            const StageUniforms& stage = analyzed[i];
            if (stage.statements.empty() || (!needsBlock && stage.builtinSamplers.empty()))
            {
                result.push_back(stages[i]);
                continue;
            }
            result.push_back(rewrite(stages[i], stage, needsBlock ? members : std::vector<std::string>{}));
        }
        return result;
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
