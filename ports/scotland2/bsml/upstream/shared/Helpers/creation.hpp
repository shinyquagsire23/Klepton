#pragma once

#include "../_config.h"
#include "../concepts.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/HoverHint.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector2.hpp"

namespace BSML::Helpers {
    /// @brief creates a text object
    /// @param parent the parent transform of the text object
    /// @param text the string to write
    /// @param anchoredPosition the position for the text
    /// @param sizeDelta the sizeDelta for the text
    /// @return the created text object
    BSML_EXPORT TMPro::TMP_Text* CreateText(System::Type* type, UnityEngine::Transform* parent, StringW text, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta = {60.0f, 10.0f});

    /// @brief creates a text object
    /// @tparam the type for the text object
    /// @param parent the parent transform of the text object
    /// @param text the string to write
    /// @param anchoredPosition the position for the text
    /// @param sizeDelta the sizeDelta for the text
    /// @return the created text object
    template<typename T = HMUI::CurvedTextMeshPro*>
    requires(std::is_convertible_v<T, TMPro::TMP_Text*>)
    T CreateText(UnityEngine::Transform* parent, StringW text, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta = {60.0f, 10.0f}) {
        return reinterpret_cast<T>(CreateText(csTypeOf(T), parent, text, anchoredPosition, sizeDelta));
    }

    /// @brief creates view controller with type
    /// @param type the type to use for the view controller
    /// @return the view controller
    BSML_EXPORT HMUI::ViewController* CreateViewController(System::Type* type);

    /// @brief creates view controller with type
    /// @tparam T the type to use for the view controller
    /// @return the view controller
    template<typename T = HMUI::ViewController*>
    requires(std::is_convertible_v<T, HMUI::ViewController*>)
    T CreateViewController() {
        return reinterpret_cast<T>(CreateViewController(csTypeOf(T)));
    }

    /// @brief creates flow coordinator with type
    /// @param type the type to use for the flow coordinator
    /// @return the flow coordinator
    BSML_EXPORT HMUI::FlowCoordinator* CreateFlowCoordinator(System::Type* type);

    /// @brief creates flow coordinator with type
    /// @tparam T the type to use for the flow coordinator
    /// @return the flow coordinator
    template<typename T = HMUI::FlowCoordinator*>
    requires(std::is_convertible_v<T, HMUI::FlowCoordinator*>)
    T CreateFlowCoordinator() {
        return reinterpret_cast<T>(CreateFlowCoordinator(csTypeOf(T)));
    }

    /// @brief adds hoverhint to given gameObject
    /// @param object the object to add the hoverhint to
    /// @param text the text to display in the hoverhint
    /// @return the hoverhint
    BSML_EXPORT HMUI::HoverHint* AddHoverHint(UnityEngine::GameObject* object, StringW text);

    /// @brief adds hoverhint to attached gameObject
    /// @tparam T the type of object
    /// @param object the object to add the hoverhint to
    /// @param text the text to display in the hoverhint
    /// @return the hoverhint
    template<BSML::Concepts::HasGameObject T>
    HMUI::HoverHint* AddHoverHint(T object, StringW text) {
        return AddHoverHint(object->get_gameObject(), text);
    }
}
