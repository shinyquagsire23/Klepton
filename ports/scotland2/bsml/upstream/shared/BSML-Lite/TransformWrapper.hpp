#pragma once

#include "../_config.h"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "beatsaber-hook/shared/utils/type-concepts.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Component.hpp"
#include "UnityEngine/GameObject.hpp"

namespace BSML::Lite {
    template<typename T>
    concept has_transform = !std::is_convertible_v<T, UnityEngine::Transform*> && requires(T t) {
        { t->get_transform() } -> std::convertible_to<UnityEngine::Transform*>;
    };

    /// @brief A wrapper for transforms, components and gameobjects to automatically be converted into a transform
    struct BSML_EXPORT TransformWrapper {
        constexpr TransformWrapper(UnityEngine::RectTransform* transform) noexcept : transform(transform) {}
        constexpr TransformWrapper(UnityEngine::Transform* transform) noexcept : transform(transform) {}

        template<typename T>
        requires(std::is_convertible_v<T, UnityEngine::Transform*>)
        TransformWrapper(T t) : TransformWrapper(static_cast<UnityEngine::Transform*>(t)) {}

        template<has_transform T>
        TransformWrapper(T t) : TransformWrapper(t->get_transform()) {}

        // il2cpp wrapper type
        explicit TransformWrapper(void* i) : transform(static_cast<UnityEngine::Transform*>(i)) {}
        constexpr inline void* convert() const noexcept { return const_cast<void*>(static_cast<const void*>(transform)); }

        UnityEngine::Transform* operator->() const noexcept { return transform; }
        operator UnityEngine::Transform*() const noexcept { return transform; }
        UnityEngine::Transform* transform;
    };
}
MARK_REF_T(BSML::Lite::TransformWrapper);

template<>
struct ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<BSML::Lite::TransformWrapper> {
    static inline Il2CppClass* get() {
        return classof(UnityEngine::Transform*);
    }
};
static_assert(sizeof(BSML::Lite::TransformWrapper) == sizeof(UnityEngine::Transform*));
