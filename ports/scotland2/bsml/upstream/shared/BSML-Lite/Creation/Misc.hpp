#pragma once

#include "../../_config.h"
#include "../GameObjectWrapper.hpp"
#include "../TransformWrapper.hpp"
#include "HMUI/HoverHint.hpp"
#include "HMUI/TextSegmentedControl.hpp"
#include "../../BSML/FloatingScreen/FloatingScreen.hpp"
#include "../../BSML/Components/ProgressBar.hpp"

namespace BSML::Lite {
    /// @brief Adds a hover hint to an object so that when it is hovered a message displays
    /// @param gameObject the object to add it to
    /// @param text the text to display
    /// @return the created hoverhint
    BSML_EXPORT HMUI::HoverHint* AddHoverHint(const GameObjectWrapper& parent, StringW text);

    /// @brief creates a floating screen
    /// @param screenSize the size of the screen
    /// @param position position of the screen
    /// @param rotation rotation of the screen
    /// @param curvatureRadius curvedness
    /// @param hasBackground is there a background
    /// @param createHandle whether there should be a handle
    /// @param handleSide side of the handle
    /// @return created object to parent to
    BSML_EXPORT BSML::FloatingScreen* CreateFloatingScreen(UnityEngine::Vector2 screenSize, UnityEngine::Vector3 position, UnityEngine::Vector3 rotation, float curvatureRadius = 0.0f, bool hasBackground = true, bool createHandle = true, BSML::Side handleSide = BSML::Side::Full);

    /// @brief creates a popup rainbow loading bar thats not attatched to any UI
    /// @param position bars position
    /// @param rotation bars rotation
    /// @param scale the size of the progress bar
    /// @param headerTextVal the main text, in rainbow
    /// @param subText1 subtext 1, text above the header
    /// @param subText2 subtext 2, text above subtext 1
    /// @return the created progress bar
    BSML_EXPORT BSML::ProgressBar* CreateProgressBar(UnityEngine::Vector3 position, UnityEngine::Vector3 rotation, UnityEngine::Vector3 scale,  StringW headerText, StringW subText1 = "", StringW subText2 = "");

    /// @brief creates a popup rainbow loading bar thats not attatched to any UI
    /// @param position bars position
    /// @param rotation bars rotation
    /// @param headerTextVal the main text, in rainbow
    /// @param subText1 subtext 1, text above the header
    /// @param subText2 subtext 2, text above subtext 1
    /// @return the created progress bar
    static inline BSML::ProgressBar* CreateProgressBar(UnityEngine::Vector3 position, UnityEngine::Vector3 rotation, StringW headerText, StringW subText1 = "", StringW subText2 = "") {
        return CreateProgressBar(position, rotation, {1, 1, 1}, headerText, subText1, subText2);
    }

    /// @brief creates a popup rainbow loading bar thats not attatched to any UI
    /// @param position bars position
    /// @param headerTextVal the main text, in rainbow
    /// @param subText1 subtext 1, text above the header
    /// @param subText2 subtext 2, text above subtext 1
    /// @return the created progress bar
    static inline BSML::ProgressBar* CreateProgressBar(UnityEngine::Vector3 position, StringW headerText, StringW subText1 = "", StringW subText2 = "") {
        return CreateProgressBar(position, {0, 0, 0}, {1, 1, 1}, headerText, subText1, subText2);
    }

    /// @brief creates a text segmented control like the one on the gameplay setup view controller
    /// @param parent what to parent it to
    /// @param anchoredPosition the position
    /// @param sizeDelta the sizeDelta
    /// @param values list of text values to give to the controller
    /// @param onCellWithIdxClicked callback called when a cell is clicked
    /// @return the created text segmented control
    BSML_EXPORT HMUI::TextSegmentedControl* CreateTextSegmentedControl(const TransformWrapper& parent, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta, std::span<std::string_view> values, std::function<void(int)> onCellWithIdxClicked = nullptr);

    /// @brief creates a text segmented control like the one on the gameplay setup view controller
    /// @param parent what to parent it to
    /// @param sizeDelta the sizeDelta
    /// @param values list of text values to give to the controller
    /// @param onCellWithIdxClicked callback called when a cell is clicked
    /// @return the created text segmented control
    template<typename T>
    requires(std::is_constructible_v<std::span<std::string_view>, T>)
    static inline HMUI::TextSegmentedControl* CreateTextSegmentedControl(const TransformWrapper& parent, UnityEngine::Vector2 sizeDelta, std::span<std::string_view> values, std::function<void(int)> onCellWithIdxClicked = nullptr) {
        return CreateTextSegmentedControl(parent, {0, 0}, sizeDelta, std::span<std::string_view>(values), onCellWithIdxClicked);
    }

    /// @brief creates a text segmented control like the one on the gameplay setup view controller
    /// @param parent what to parent it to
    /// @param values list of text values to give to the controller
    /// @param onCellWithIdxClicked callback called when a cell is clicked
    /// @return the created text segmented control
    template<typename T>
    requires(std::is_constructible_v<std::span<std::string_view>, T>)
    static inline HMUI::TextSegmentedControl* CreateTextSegmentedControl(const TransformWrapper& parent, T values, std::function<void(int)> onCellWithIdxClicked) {
        return CreateTextSegmentedControl(parent, {0, 0}, {90.0f, 10.0f}, std::span<std::string_view>(values), onCellWithIdxClicked);
    }

    /// @brief creates a text segmented control like the one on the gameplay setup view controller
    /// @param parent what to parent it to
    /// @param onCellWithIdxClicked callback called when a cell is clicked
    /// @return the created text segmented control
    static inline HMUI::TextSegmentedControl* CreateTextSegmentedControl(const TransformWrapper& parent, std::function<void(int)> onCellWithIdxClicked) {
        return CreateTextSegmentedControl(parent, {0, 0}, {90.0f, 10.0f}, {}, onCellWithIdxClicked);
    }

    /// @brief creates a Unity canvas gameobject that's setup for beat saber UI
    /// @return the created canvas gameobject
    BSML_EXPORT UnityEngine::GameObject* CreateCanvas();
}
