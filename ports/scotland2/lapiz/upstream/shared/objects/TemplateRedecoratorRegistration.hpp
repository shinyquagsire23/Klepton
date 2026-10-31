#pragma once

#include "custom-types/shared/macros.hpp"
#include "RedecoratorRegistration.hpp"

DECLARE_CLASS_CUSTOM(Lapiz::Objects, TemplateRedecoratorRegistration, RedecoratorRegistration) {
    DECLARE_INSTANCE_METHOD(System::Object*, Redecorate, System::Object* value);
    DECLARE_CTOR(ctor, StringW contract, System::Type* prefabType, System::Type* containerType, int priority, bool chain);

    public:
        template<typename TPrefabType, typename TParentType>
        static TemplateRedecoratorRegistration* Make(std::string_view contract, std::function<TPrefabType(TPrefabType)> redecorateCall, int priority = 0, bool chain = true) {
            auto res = TemplateRedecoratorRegistration::New_ctor(contract, csTypeOf(TPrefabType), csTypeOf(TParentType), priority, chain);
            res->_redecorateCall = [redecorateCall](System::Object* value) -> System::Object* {
                return redecorateCall(reinterpret_cast<TPrefabType>(value));
            };
            return res;
        }

    private:
        std::function<System::Object*(System::Object*)> _redecorateCall;
};