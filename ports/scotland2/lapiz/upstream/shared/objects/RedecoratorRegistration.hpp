#pragma once

#include "custom-types/shared/macros.hpp"
#include "../macros.hpp"

DECLARE_CLASS_CODEGEN(Lapiz::Objects, RedecoratorRegistration, System::Object) {
    DECLARE_INSTANCE_FIELD_PRIVATE(bool, _chain);
    DECLARE_INSTANCE_FIELD_PRIVATE(int, _priority);
    DECLARE_INSTANCE_FIELD_PRIVATE(StringW, _contract);

    DECLARE_INSTANCE_FIELD_PRIVATE(System::Type*, _prefabType);
    DECLARE_INSTANCE_FIELD_PRIVATE(System::Type*, _containerType);
    DECLARE_INSTANCE_FIELD_PRIVATE(void*, minfo);

    DECLARE_INSTANCE_METHOD(bool, get_chain);
    DECLARE_INSTANCE_METHOD(int, get_priority);
    DECLARE_INSTANCE_METHOD(StringW, get_contract);

    DECLARE_INSTANCE_METHOD(System::Type*, get_prefabType);
    DECLARE_INSTANCE_METHOD(System::Type*, get_containerType);

    DECLARE_INSTANCE_METHOD(System::Object*, Redecorate_internal, System::Object* value);
    DECLARE_CTOR(ctor, StringW contract, System::Type* prefabType, System::Type* containerType, int priority, bool chain);

    public:
        __declspec(property(get=get_chain)) bool Chain;
        __declspec(property(get=get_priority)) int Priority;
        __declspec(property(get=get_contract)) StringW Contract;
        __declspec(property(get=get_prefabType)) System::Type* PrefabType;
        __declspec(property(get=get_containerType)) System::Type* ContainerType;
};