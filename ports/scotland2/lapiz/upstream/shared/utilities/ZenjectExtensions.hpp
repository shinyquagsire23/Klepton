#pragma once

#include "../_config.h"
#include "Zenject/FromBinder.hpp"
#include "Zenject/ScopeConcreteIdArgConditionCopyNonLazyBinder.hpp"
#include "Zenject/NameTransformScopeConcreteIdArgConditionCopyNonLazyBinder.hpp"
#include "Zenject/GameObjectCreationParameters.hpp"
#include "Zenject/InjectContext.hpp"

#include "UnityEngine/EventSystems/BaseRaycaster.hpp"
#include "UnityEngine/EventSystems/PointerEventData.hpp"
#include "UnityEngine/EventSystems/RaycastResult.hpp"
#include "UnityEngine/Camera.hpp"

#include "custom-types/shared/macros.hpp"

namespace Lapiz::Zenject::ZenjectExtensions {
    /// @brief Binds a view controller to the container.
    /// @param binder The binder.
    /// @param onInstantiated The callback when the view controller is instantiated.
    /// @return
    LAPIZ_EXPORT ::Zenject::ScopeConcreteIdArgConditionCopyNonLazyBinder* FromNewComponentAsViewController(::Zenject::FromBinder* binder, std::function<void(::Zenject::InjectContext*, System::Object*)> onInstantiated = nullptr);

    /// @brief Creates a new component on a new GameObject
    /// @param binder
    /// @param gameObjectInfo
    /// @return
    LAPIZ_EXPORT ::Zenject::NameTransformScopeConcreteIdArgConditionCopyNonLazyBinder* FromNewComponentOnNewGameObject(::Zenject::FromBinder* binder, ::Zenject::GameObjectCreationParameters* gameObjectInfo);

    /// @brief Creates a new component on a new GameObject
    /// @param binder
    /// @return
    static inline ::Zenject::NameTransformScopeConcreteIdArgConditionCopyNonLazyBinder* FromNewComponentOnNewGameObject(::Zenject::FromBinder* binder) {
        return FromNewComponentOnNewGameObject(binder,  ::Zenject::GameObjectCreationParameters::New_ctor());
    }
}

DECLARE_CLASS_CODEGEN(Lapiz::Zenject::ZenjectExtensions, DummyRaycaster, UnityEngine::EventSystems::BaseRaycaster) {
    DECLARE_OVERRIDE_METHOD_MATCH(UnityEngine::Camera*, get_eventCamera, &::UnityEngine::EventSystems::BaseRaycaster::get_eventCamera);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Raycast, &::UnityEngine::EventSystems::BaseRaycaster::Raycast, UnityEngine::EventSystems::PointerEventData* pointerEventData, ListW<UnityEngine::EventSystems::RaycastResult> resultAppendList);
};