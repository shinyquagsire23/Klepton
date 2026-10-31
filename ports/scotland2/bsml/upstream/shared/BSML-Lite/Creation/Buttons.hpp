#pragma once

#include "../../_config.h"
#include "../TransformWrapper.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/Vector2.hpp"

#define DEFAULT_BUTTONTEMPLATE "PracticeButton"

namespace BSML::Lite {
    /// @brief Create a button with text
    /// @param parent what to parent it to
    /// @param buttonText the text on the button
    /// @param buttonTemplate button template to use for instantiation
    /// @param anchoredPosition position of the anchor relative to the parent
    /// @param sizeDelta how much smaller this thing is relative to the parent
    /// @param onClick what to run when it's clicked
    /// @return created button
    BSML_EXPORT UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, const std::string_view& buttonTemplate, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta, std::function<void()> onClick = nullptr);

    /// @brief Create a button with text
    /// @param parent what to parent it to
    /// @param buttonText the text on the button
    /// @param buttonTemplate button template to use for instantiation
    /// @param anchoredPosition position of the anchor relative to the parent
    /// @param onClick what to run when it's clicked
    /// @return created button
    static inline UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, const std::string_view& buttonTemplate, UnityEngine::Vector2 anchoredPosition, std::function<void()> onClick = nullptr) {
        return CreateUIButton(parent, buttonText, buttonTemplate, anchoredPosition, {0, 0}, onClick);
    }

    /// @brief Create a button with text
    /// @param parent what to parent it to
    /// @param buttonText the text on the button
    /// @param buttonTemplate button template to use for instantiation
    /// @param onClick what to run when it's clicked
    /// @return created button
    static inline UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, const std::string_view& buttonTemplate, std::function<void()> onClick = nullptr) {
        return CreateUIButton(parent, buttonText, buttonTemplate, {0, 0}, {0, 0}, onClick);
    }

    /// @brief Create a button with text
    /// @param parent what to parent it to
    /// @param buttonText the text on the button
    /// @param anchoredPosition position of the anchor relative to the parent
    /// @param sizeDelta how much smaller this thing is relative to the parent
    /// @param onClick what to run when it's clicked
    /// @return created button
    BSML_EXPORT UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta, std::function<void()> onClick = nullptr);

    /// @brief Create a button with text
    /// @param parent what to parent it to
    /// @param buttonText the text on the button
    /// @param anchoredPosition position of the anchor relative to the parent
    /// @param onClick what to run when it's clicked
    /// @return created button
    static inline UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, UnityEngine::Vector2 anchoredPosition, std::function<void()> onClick = nullptr) {
        return CreateUIButton(parent, buttonText, anchoredPosition, {0, 0}, onClick);
    }

    /// @brief Create a button with text
    /// @param parent what to parent it to
    /// @param buttonText the text on the button
    /// @param onClick what to run when it's clicked
    /// @return created button
    static inline UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, std::function<void()> onClick = nullptr) {
        return CreateUIButton(parent, buttonText, UnityEngine::Vector2({0, 0}), {0, 0}, onClick);
    }

    /// @brief set the button text on the passed in button
    /// @param button the button to change the text on
    /// @param text the text to set
    BSML_EXPORT void SetButtonText(UnityEngine::UI::Button* button, StringW text);

    /// @brief set the button text size on the passed in button
    /// @param button the button to change the text size on
    /// @param fontSize the font size to change the text to
    BSML_EXPORT void SetButtonTextSize(UnityEngine::UI::Button* button, float fontSize);

    /// @brief set the button wordwrapping on the passed in button
    /// @param button the button to change the text size on
    /// @param enableWordWrapping whether to allow wordwrapping
    BSML_EXPORT void ToggleButtonWordWrapping(UnityEngine::UI::Button* button, bool enableWordWrapping);

    /// @brief set the icon on the passed in button
    /// @param button the button to change icon of
    /// @param icon the icon to set
    BSML_EXPORT void SetButtonIcon(UnityEngine::UI::Button* button, UnityEngine::Sprite* icon);

    /// @brief set the background on the passed in button
    /// @param button the button to change the background on
    /// @param icon the background to set
    BSML_EXPORT void SetButtonBackground(UnityEngine::UI::Button* button, UnityEngine::Sprite* background);

    /// @brief sets the sprites for a given button
    /// @param button the button to swap sprites for
    /// @param inactive, for when not hovering over the button
    /// @param active for when hovering over the button
    BSML_EXPORT void SetButtonSprites(UnityEngine::UI::Button* button, UnityEngine::Sprite* inactive, UnityEngine::Sprite* active);
}
