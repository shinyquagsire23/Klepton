#pragma once

#include <concepts>
#include <type_traits>

#include "beatsaber-hook/shared/utils/typedefs.h"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"

namespace BSML::Concepts {
    template<typename T, typename U>
    concept BSMLConvertible = std::is_convertible_v<T, U>;

    template<typename T>
    concept HasGameObject = !BSMLConvertible<T, UnityEngine::GameObject*> && requires (T a) { {a->get_gameObject() } -> BSMLConvertible<UnityEngine::GameObject*>; };

    template<typename T>
    concept HasTransform = !BSMLConvertible<T, UnityEngine::Transform*> && requires (T a) { {a->get_transform() } -> BSMLConvertible<UnityEngine::Transform*>; };
}
