#pragma once

#include <glslang/glslang/MachineIndependent/iomapper.h>

namespace ShaderCross {

// Metal stores ordinary uniforms in a buffer, not in texture slots. The default
// glslang resolver reserves descriptor bindings for them, leaving holes in the
// texture indices emitted by SPIRV-Cross's enable_decoration_binding mode.
class MetalBindingResolver final : public glslang::TDefaultIoResolverBase {
public:
    explicit MetalBindingResolver(const glslang::TIntermediate& intermediate)
        : TDefaultIoResolverBase(intermediate) {}

    bool validateBinding(EShLanguage, glslang::TVarEntryInfo&) override { return true; }

    void reserveExplicitBindings(glslang::TIntermediate& stage) {
        struct Collector : glslang::TIntermTraverser {
            MetalBindingResolver& resolver;
            explicit Collector(MetalBindingResolver& resolver) : resolver(resolver) {}
            void visitSymbol(glslang::TIntermSymbol* symbol) override {
                if (!symbol->getQualifier().isUniformOrBuffer() || !symbol->getQualifier().hasBinding()) return;
                glslang::TVarEntryInfo entry{};
                entry.symbol = symbol;
                resolver.resolveBinding(EShLangCount, entry);
            }
        } collector(*this);
        if (stage.getTreeRoot()) stage.getTreeRoot()->traverse(&collector);
    }

    glslang::TResourceType getResourceType(const glslang::TType& type) override {
        if (isImageType(type)) return glslang::EResImage;
        if (isTextureType(type)) return glslang::EResTexture;
        if (isSamplerType(type)) return glslang::EResSampler;
        if (isSsboType(type)) return glslang::EResSsbo;
        if (isUboType(type) && type.getBasicType() == glslang::EbtBlock) return glslang::EResUbo;
        return glslang::EResCount;
    }

    int resolveBinding(EShLanguage, glslang::TVarEntryInfo& entry) override {
        const auto& type = entry.symbol->getType();
        const auto resource = getResourceType(type);
        entry.newBinding = -1;
        if (resource == glslang::EResCount) return entry.newBinding;

        const int set = getLayoutSet(type);
        // Metal buffers and textures have independent argument tables. Keep
        // images/combined samplers together, and uniform/storage buffers together.
        const int slotsKey = set * 2 + (resource == glslang::EResUbo || resource == glslang::EResSsbo ? 1 : 0);
        const auto& resourceName = type.getBasicType() == glslang::EbtBlock ? type.getTypeName() : entry.symbol->getName();
        const std::string name = std::to_string(resource) + ":" + resourceName.c_str();
        auto previous = bindings.find(name);
        if (previous != bindings.end()) return entry.newBinding = previous->second;
        // Metal arrays occupy consecutive argument indices, including when the
        // input SPIR-V was generated without OpenGL client semantics.
        const int count = type.isSizedArray() ? type.getCumulativeArraySize() : 1;
        const int base = getBaseBinding(resource, set);
        if (type.getQualifier().hasBinding()) {
            entry.newBinding = reserveSlot(slotsKey, base + type.getQualifier().layoutBinding, count);
        } else if (entry.live && doAutoBindingMapping()) {
            entry.newBinding = getFreeSlot(slotsKey, base, count);
        }
        if (entry.newBinding >= 0) bindings[name] = entry.newBinding;
        return entry.newBinding;
    }

private:
    std::unordered_map<std::string, int> bindings;
};

} // namespace ShaderCross
