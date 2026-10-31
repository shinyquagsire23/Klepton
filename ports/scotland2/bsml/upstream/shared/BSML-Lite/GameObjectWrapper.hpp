#pragma once

#include "../_config.h"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "beatsaber-hook/shared/utils/type-concepts.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Component.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Transform.hpp"

namespace BSML::Lite {
    template<typename T>
    concept has_gameObject = !std::is_convertible_v<T, UnityEngine::GameObject*> && requires(T t) {
        { t->get_gameObject() } -> std::convertible_to<UnityEngine::GameObject*>;
    };

    /// @brief A wrapper for transforms, components and gameobjects to automatically be converted into a gameObject
    struct BSML_EXPORT GameObjectWrapper {
        constexpr GameObjectWrapper(UnityEngine::GameObject* gameObject) noexcept : gameObject(gameObject) {}

        template<has_gameObject T>
        GameObjectWrapper(T t) : GameObjectWrapper(t->get_gameObject()) {}

        template<typename T>
        requires(std::is_convertible_v<T, UnityEngine::GameObject*>)
        GameObjectWrapper(T t) : GameObjectWrapper(static_cast<UnityEngine::GameObject*>(t)) {}

        // il2cpp wrapper type
        explicit GameObjectWrapper(void* i) : gameObject(static_cast<UnityEngine::GameObject*>(i)) {}
        constexpr inline void* convert() const noexcept { return const_cast<void*>(static_cast<const void*>(gameObject)); }

        UnityEngine::GameObject* operator->() const noexcept { return const_cast<UnityEngine::GameObject*>(gameObject); }
        operator UnityEngine::GameObject*() const noexcept { return const_cast<UnityEngine::GameObject*>(gameObject); }
        UnityEngine::GameObject* gameObject;
    };
}
MARK_REF_T(BSML::Lite::GameObjectWrapper);

template<>
struct ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<BSML::Lite::GameObjectWrapper> {
    static inline Il2CppClass* get() {
        return classof(UnityEngine::GameObject*);
    }
};
static_assert(sizeof(BSML::Lite::GameObjectWrapper) == sizeof(UnityEngine::GameObject*));
