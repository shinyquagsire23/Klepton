#include "BSML/Components/ExternalComponents.hpp"
#include "logging.hpp"

DEFINE_TYPE(BSML, ExternalComponents);

namespace BSML {
    void ExternalComponents::ctor() {
        components = ListW<UnityEngine::Component*>::New();
    }

    void ExternalComponents::Add(UnityEngine::Component* component) {
        // runtime instance null check
        if (!static_cast<const void*>(this)) throw cordl_internals::NullException("Retrieving component on nullptr external components!");

        components->Add(component);
    }

    UnityEngine::Component* ExternalComponents::GetByType(System::Type* type) const {
        return GetByType(reinterpret_cast<Il2CppReflectionType*>(type));
    }

    UnityEngine::Component* ExternalComponents::GetByType(Il2CppReflectionType* type) const {
        // runtime instance null check
        if (!static_cast<const void*>(this)) throw cordl_internals::NullException("Retrieving component on nullptr external components!");
        if (!type) return nullptr;

        auto klass = il2cpp_functions::class_from_system_type(type);
        INFO("Getting {}::{}", klass->namespaze, klass->name);
        for (auto component : components) {
            if (il2cpp_functions::class_is_assignable_from(klass, component->klass)) {
                return component;
            }
        }

        return nullptr;
    }
}
