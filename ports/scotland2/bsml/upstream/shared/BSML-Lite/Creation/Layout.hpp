#pragma once

#include "../../_config.h"
#include "../TransformWrapper.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "UnityEngine/UI/HorizontalLayoutGroup.hpp"
#include "UnityEngine/UI/GridLayoutGroup.hpp"
#include "HMUI/StackLayoutGroup.hpp"
#include "../../BSML/Components/ModalView.hpp"

namespace BSML::Lite {

    // @brief Creates a vertical layout group
    /// @param parent what to parent it to
    /// @return the created layout
    BSML_EXPORT UnityEngine::UI::VerticalLayoutGroup* CreateVerticalLayoutGroup(const TransformWrapper& parent);

    // @brief Creates a horizontal layout group
    /// @param parent what to parent it to
    /// @return the created layout
    BSML_EXPORT UnityEngine::UI::HorizontalLayoutGroup* CreateHorizontalLayoutGroup(const TransformWrapper& parent);

    // @brief Creates a grid layout group
    /// @param parent what to parent it to
    /// @return the created layout
    BSML_EXPORT UnityEngine::UI::GridLayoutGroup* CreateGridLayoutGroup(const TransformWrapper& parent);

    // @brief Creates a stack layout group
    /// @param parent what to parent it to
    /// @return the created layout
    BSML_EXPORT HMUI::StackLayoutGroup* CreateStackLayoutGroup(const TransformWrapper& parent);

    /// @brief Create a scrollable settings container
    /// @param parent what to parent it to
    /// @return created container
    BSML_EXPORT UnityEngine::GameObject* CreateScrollableSettingsContainer(const TransformWrapper& parent);

    /// @brief Creates a scroll view
    /// @param parent what to parent it to
    /// @return created container which you should add your content to
    BSML_EXPORT UnityEngine::GameObject* CreateScrollView(const TransformWrapper& parent);

    /// @brief creates a modal that can be used to display information
    /// @param parent what to parent it to
    /// @param sizeDelta size of the object
    /// @param anchoredPosition position of the modal
    /// @param onBlockerClicked callback that gets called when clicking next to the modal, leaving it empty makes it just dismiss the modal
    /// @param dismissOnBlockerClicked whether to auto dismiss when the blocker (outside) is clicked
    /// @return created modal
    BSML_EXPORT BSML::ModalView* CreateModal(const TransformWrapper& parent, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta, std::function<void()> onBlockerClicked, bool dismissOnBlockerClicked = true);

    /// @brief creates a modal that can be used to display information
    /// @param parent what to parent it to
    /// @param sizeDelta size of the object
    /// @param onBlockerClicked callback that gets called when clicking next to the modal, leaving it empty makes it just dismiss the modal
    /// @param dismissOnBlockerClicked whether to auto dismiss when the blocker (outside) is clicked
    /// @return created modal
    static inline BSML::ModalView* CreateModal(const TransformWrapper& parent, UnityEngine::Vector2 sizeDelta, std::function<void()> onBlockerClicked, bool dismissOnBlockerClicked = true) {
        return CreateModal(parent, {0, 0}, sizeDelta, onBlockerClicked, dismissOnBlockerClicked);
    }

    /// @brief creates a modal that can be used to display information
    /// @param parent what to parent it to
    /// @param onBlockerClicked callback that gets called when clicking next to the modal, leaving it empty makes it just dismiss the modal
    /// @param dismissOnBlockerClicked whether to auto dismiss when the blocker (outside) is clicked
    /// @return created modal
    static inline BSML::ModalView* CreateModal(const TransformWrapper& parent, std::function<void()> onBlockerClicked, bool dismissOnBlockerClicked = true) {
        return CreateModal(parent, {0, 0}, {30.0f, 40.0f}, onBlockerClicked, dismissOnBlockerClicked);
    }

    /// @brief creates a modal that can be used to display information
    /// @param parent what to parent it to
    /// @param dismissOnBlockerClicked whether to auto dismiss when the blocker (outside) is clicked
    /// @return created modal
    static inline BSML::ModalView* CreateModal(const TransformWrapper& parent, bool dismissOnBlockerClicked = true) {
        return CreateModal(parent, {0, 0}, {30.0f, 40.0f}, nullptr, dismissOnBlockerClicked);
    }

    /// @brief creates a scrollable container for the given modal, with exact fit
    /// @param modal the modal to create a container for
    /// @return GameObject container
    BSML_EXPORT UnityEngine::GameObject* CreateScrollableModalContainer(BSML::ModalView* modal);

    /// @brief Creates a container which fits the modifier tab well
    /// @param parent what to parent it to
    /// @return vertical layout group to parent things to
    BSML_EXPORT UnityEngine::UI::VerticalLayoutGroup* CreateModifierContainer(const TransformWrapper& parent);
}
