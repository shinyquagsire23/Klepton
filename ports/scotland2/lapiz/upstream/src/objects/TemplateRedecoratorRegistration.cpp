#include "objects/TemplateRedecoratorRegistration.hpp"

#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

DEFINE_TYPE(Lapiz::Objects, TemplateRedecoratorRegistration);

namespace Lapiz::Objects {
    System::Object* TemplateRedecoratorRegistration::Redecorate(System::Object* value) {
        if (_redecorateCall) return _redecorateCall(value);
        else return value;
    }

    void TemplateRedecoratorRegistration::ctor(StringW contract, System::Type* prefabType, System::Type* containerType, int priority, bool chain) {
        static auto baseKlass = classof(RedecoratorRegistration*);
        // TODO: tell sc2ad about invoke base ctor ExtractIndependenType not having il2cpp_utils:: in front!
        INVOKE_BASE_CTOR(baseKlass, contract, prefabType, containerType, priority, chain);
    }
}