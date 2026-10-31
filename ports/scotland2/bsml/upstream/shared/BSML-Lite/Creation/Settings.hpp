#pragma once

#include "../../_config.h"
#include "../TransformWrapper.hpp"
#include "HMUI/InputFieldView.hpp"
#include "UnityEngine/UI/Toggle.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"

#include "../../BSML/Components/Settings/IncrementSetting.hpp"
#include "../../BSML/Components/Settings/SliderSetting.hpp"
#include "../../BSML/Components/Settings/ColorSetting.hpp"
#include "../../BSML/Components/Settings/DropdownListSetting.hpp"
#include "../../BSML/Components/Settings/ToggleSetting.hpp"
#include "../../BSML/Components/ModalColorPicker.hpp"

namespace BSML::Lite {
    /// @brief creates a string setting allowing users to input a string with a keyboard
    /// @param parent what to parent it to
    /// @param settingsName the label to give to the setting
    /// @param currentValue the current string value it is
    /// @param anchoredPosition position of the string setting
    /// @param keyboardPositionOffset offset of the keyboard in 3d space
    /// @param onValueChange callback ran when the value changes
    /// @return the created string setting
    BSML_EXPORT HMUI::InputFieldView* CreateStringSetting(const TransformWrapper& parent, StringW settingsName, StringW currentValue, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector3 keyboardPositionOffset, std::function<void(StringW)> onValueChange = nullptr);

    /// @brief creates a string setting allowing users to input a string with a keyboard
    /// @param parent what to parent it to
    /// @param settingsName the label to give to the setting
    /// @param currentValue the current string value it is
    /// @param anchoredPosition position of the string setting
    /// @param onValueChange callback ran when the value changes
    /// @return the created string setting
    static inline HMUI::InputFieldView* CreateStringSetting(const TransformWrapper& parent, StringW settingsName, StringW currentValue, UnityEngine::Vector2 anchoredPosition, std::function<void(StringW)> onValueChange = nullptr) {
        return CreateStringSetting(parent, settingsName, currentValue, anchoredPosition, {0, 0, 0}, onValueChange);
    }

    /// @brief creates a string setting allowing users to input a string with a keyboard
    /// @param parent what to parent it to
    /// @param settingsName the label to give to the setting
    /// @param currentValue the current string value it is
    /// @param onValueChange callback ran when the value changes
    /// @return the created string setting
    static inline HMUI::InputFieldView* CreateStringSetting(const TransformWrapper& parent, StringW settingsName, StringW currentValue, std::function<void(StringW)> onValueChange = nullptr) {
        return CreateStringSetting(parent, settingsName, currentValue, {0, 0}, {0, 0, 0}, onValueChange);
    }

    /// @brief Creates a toggle that looks like the modifier buttons seen in the gameplay setup menu
    /// @param parent what to parent it to
    /// @param buttonText the text to display on the button
    /// @param currentValue is the toggle true or false at this moment
    /// @param iconSprite the sprite for the icon
    /// @param onClick what to run when the button is clicked
    /// @param anchoredPosition the position of the button
    BSML_EXPORT UnityEngine::UI::Toggle* CreateModifierButton(const TransformWrapper& parent, StringW buttonText, bool currentValue, UnityEngine::Sprite* iconSprite, std::function<void(bool)> onClick = nullptr, UnityEngine::Vector2 anchoredPosition = {0, 0});

    /// @brief Creates a toggle that looks like the modifier buttons seen in the gameplay setup menu
    /// @param parent what to parent it to
    /// @param buttonText the text to display on the button
    /// @param currentValue is the toggle true or false at this moment
    /// @param onClick what to run when the button is clicked
    /// @param anchoredPosition the position of the button
    static inline UnityEngine::UI::Toggle* CreateModifierButton(const TransformWrapper& parent, StringW buttonText, bool currentValue, std::function<void(bool)> onClick = nullptr, UnityEngine::Vector2 anchoredPosition = {0, 0}) {
        return CreateModifierButton(parent, buttonText, currentValue, nullptr, onClick, anchoredPosition);
    }

    /// @brief Creates an incerement setting, meaning a float value with arrows to go up and down
    /// @param parent what to parent it to
    /// @param label label to give to the setting
    /// @param decimals amount of decimals to show
    /// @param increment what to increment/decrement the value by
    /// @param currentValue the starting value
    /// @param hasMin whether or not the minimum value should be enforced
    /// @param hasMax whether or not the maximum value should be enforced
    /// @param minValue the minimum value the setting is allowed to be
    /// @param maxValue the maximum value the setting is allowed to be
    /// @param anchoredPosition the position of the setting
    /// @param onValueChange what to run when the value is changed
    /// @return the created increment setting
    BSML_EXPORT BSML::IncrementSetting* CreateIncrementSetting(const TransformWrapper& parent, StringW label, int decimals, float increment, float currentValue, bool hasMin, bool hasMax, float minValue, float maxValue, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange = nullptr);

    /// @brief Creates an incerement setting, meaning a float value with arrows to go up and down
    /// @param parent what to parent it to
    /// @param label label to give to the setting
    /// @param decimals amount of decimals to show
    /// @param increment what to increment/decrement the value by
    /// @param currentValue the starting value
    /// @param hasMin whether or not the minimum value should be enforced
    /// @param hasMax whether or not the maximum value should be enforced
    /// @param minValue the minimum value the setting is allowed to be
    /// @param maxValue the maximum value the setting is allowed to be
    /// @param onValueChange what to run when the value is changed
    /// @return the created increment setting
    static inline BSML::IncrementSetting* CreateIncrementSetting(const TransformWrapper& parent, StringW label, int decimals, float increment, float currentValue, bool hasMin, bool hasMax, float minValue, float maxValue, std::function<void(float)> onValueChange = nullptr) {
        return CreateIncrementSetting(parent, label, decimals, increment, currentValue, hasMin, hasMax, minValue, maxValue, {0, 0}, onValueChange);
    }

    /// @brief Creates an incerement setting, meaning a float value with arrows to go up and down
    /// @param parent what to parent it to
    /// @param label label to give to the setting
    /// @param decimals amount of decimals to show
    /// @param increment what to increment/decrement the value by
    /// @param currentValue the starting value
    /// @param minValue the minimum value the setting is allowed to be
    /// @param maxValue the maximum value the setting is allowed to be
    /// @param anchoredPosition the position of the setting
    /// @param onValueChange what to run when the value is changed
    /// @return the created increment setting
    static inline BSML::IncrementSetting* CreateIncrementSetting(const TransformWrapper& parent, StringW label, int decimals, float increment, float currentValue, float minValue, float maxValue, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange = nullptr) {
        return CreateIncrementSetting(parent, label, decimals, increment, currentValue, true, true, minValue, maxValue, anchoredPosition, onValueChange);
    }

    /// @brief Creates an incerement setting, meaning a float value with arrows to go up and down
    /// @param parent what to parent it to
    /// @param label label to give to the setting
    /// @param decimals amount of decimals to show
    /// @param increment what to increment/decrement the value by
    /// @param currentValue the starting value
    /// @param minValue the minimum value the setting is allowed to be
    /// @param maxValue the maximum value the setting is allowed to be
    /// @param onValueChange what to run when the value is changed
    /// @return the created increment setting
    static inline BSML::IncrementSetting* CreateIncrementSetting(const TransformWrapper& parent, StringW label, int decimals, float increment, float currentValue, float minValue, float maxValue, std::function<void(float)> onValueChange = nullptr) {
        return CreateIncrementSetting(parent, label, decimals, increment, currentValue, true, true, minValue, maxValue, {0, 0}, onValueChange);
    }

    /// @brief Creates an incerement setting, meaning a float value with arrows to go up and down
    /// @param parent what to parent it to
    /// @param label label to give to the setting
    /// @param decimals amount of decimals to show
    /// @param increment what to increment/decrement the value by
    /// @param currentValue the starting value
    /// @param anchoredPosition the position of the setting
    /// @param onValueChange what to run when the value is changed
    /// @return the created increment setting
    static inline BSML::IncrementSetting* CreateIncrementSetting(const TransformWrapper& parent, StringW label, int decimals, float increment, float currentValue, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange = nullptr) {
        return CreateIncrementSetting(parent, label, decimals, increment, currentValue, false, false, std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), anchoredPosition, onValueChange);
    }

    /// @brief Creates an incerement setting, meaning a float value with arrows to go up and down
    /// @param parent what to parent it to
    /// @param label label to give to the setting
    /// @param decimals amount of decimals to show
    /// @param increment what to increment/decrement the value by
    /// @param currentValue the starting value
    /// @param onValueChange what to run when the value is changed
    /// @return the created increment setting
    static inline BSML::IncrementSetting* CreateIncrementSetting(const TransformWrapper& parent, StringW label, int decimals, float increment, float currentValue, std::function<void(float)> onValueChange = nullptr) {
        return CreateIncrementSetting(parent, label, decimals, increment, currentValue, false, false, std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), {0, 0}, onValueChange);
    }

    /// @brief Creates a slider setting
    /// @param parent what to parent it to
    /// @param text the text displayed in front to let the user know what they are editing
    /// @param increment the increment each "tick" should do
    /// @param currentValue the initial value it should be
    /// @param minValue the minimum value for the slider
    /// @param maxValue the maximum value for the slider
    /// @param applyValueTime the amount of seconds to wait after the user stopped interacting to call the callback
    /// @param anchoredPosition the position
    /// @param onValueChange callback called after the user has not interacted with the slider for applyValueTime
    /// @return the created slider
    BSML_EXPORT BSML::SliderSetting* CreateSliderSetting(const TransformWrapper& parent, StringW label, float increment, float currentValue, float minValue, float maxValue, float applyValueTime, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange = nullptr);

    /// @brief Creates a slider setting
    /// @param parent what to parent it to
    /// @param text the text displayed in front to let the user know what they are editing
    /// @param increment the increment each "tick" should do
    /// @param currentValue the initial value it should be
    /// @param minValue the minimum value for the slider
    /// @param maxValue the maximum value for the slider
    /// @param applyValueTime the amount of seconds to wait after the user stopped interacting to call the callback
    /// @param showButtons whether to show the buttons
    /// @param anchoredPosition the position
    /// @param onValueChange callback called after the user has not interacted with the slider for applyValueTime
    /// @return the created slider
    BSML_EXPORT BSML::SliderSetting* CreateSliderSetting(const TransformWrapper& parent, StringW label, float increment, float currentValue, float minValue, float maxValue, float applyValueTime, bool showButtons, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange = nullptr);

    /// @brief Creates a slider setting
    /// @param parent what to parent it to
    /// @param text the text displayed in front to let the user know what they are editing
    /// @param increment the increment each "tick" should do
    /// @param currentValue the initial value it should be
    /// @param minValue the minimum value for the slider
    /// @param maxValue the maximum value for the slider
    /// @param anchoredPosition the position
    /// @param onValueChange callback called after the user has not interacted with the slider for 1 second
    /// @return the created slider
    static inline BSML::SliderSetting* CreateSliderSetting(const TransformWrapper& parent, StringW label, float increment, float currentValue, float minValue, float maxValue, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange = nullptr) {
        return CreateSliderSetting(parent, label, increment, currentValue, minValue, maxValue, 1.0f, anchoredPosition, onValueChange);
    }

    /// @brief Creates a slider setting
    /// @param parent what to parent it to
    /// @param text the text displayed in front to let the user know what they are editing
    /// @param increment the increment each "tick" should do
    /// @param currentValue the initial value it should be
    /// @param minValue the minimum value for the slider
    /// @param maxValue the maximum value for the slider
    /// @param applyValueTime the amount of seconds to wait after the user stopped interacting to call the callback
    /// @param onValueChange callback called after the user has not interacted with the slider for applyValueTime
    static inline BSML::SliderSetting* CreateSliderSetting(const TransformWrapper& parent, StringW label, float increment, float currentValue, float minValue, float maxValue, float applyValueTime, std::function<void(float)> onValueChange = nullptr) {
        return CreateSliderSetting(parent, label, increment, currentValue, minValue, maxValue, applyValueTime, {0, 0}, onValueChange);
    }

    /// @brief Creates a slider setting
    /// @param parent what to parent it to
    /// @param text the text displayed in front to let the user know what they are editing
    /// @param increment the increment each "tick" should do
    /// @param currentValue the initial value it should be
    /// @param minValue the minimum value for the slider
    /// @param maxValue the maximum value for the slider
    /// @param onValueChange callback called after the user has not interacted with the slider for 1 second
    static inline BSML::SliderSetting* CreateSliderSetting(const TransformWrapper& parent, StringW label, float increment, float currentValue, float minValue, float maxValue, std::function<void(float)> onValueChange = nullptr) {
        return CreateSliderSetting(parent, label, increment, currentValue, minValue, maxValue, 1.0f, {0, 0}, onValueChange);
    }

    /// @brief creates a dropdown menu to select from a set of pre-known strings (like an enum)
    /// @param parent what to parent it to
    /// @param label label of the setting
    /// @param currentValue what to display as currently selected
    /// @param values the possible string values that can be displayed
    /// @param onValueChange callback ran when the value changes
    /// @return the created dropdown
    BSML_EXPORT BSML::DropdownListSetting* CreateDropdown(const TransformWrapper& parent, StringW label, StringW currentValue, std::span<std::string_view> values, std::function<void(StringW)> onValueChange = nullptr);

    /// @brief creates a dropdown menu to select from a set of pre-known strings (like an enum)
    /// @param parent what to parent it to
    /// @param label label of the setting
    /// @param currentValue what to display as currently selected
    /// @param values the possible string values that can be displayed
    /// @param onValueChange callback ran when the value changes
    /// @return the created dropdown
    template<typename T>
    requires(std::is_constructible_v<std::span<std::string_view>, T> && !std::is_same_v<std::span<std::string_view>, T>)
    BSML::DropdownListSetting* CreateDropdown(const TransformWrapper& parent, StringW label, StringW currentValue, T values, std::function<void(StringW)> onValueChange = nullptr) {
        return CreateDropdown(parent, label, currentValue, std::span<std::string_view>(values), onValueChange);
    }

    /// @brief creates a color picker
    /// @param parent what to parent it to
    /// @param text the label to display
    /// @param defaultColor the starting color
    /// @param onDone the callback to call when color picking is done
    /// @param onCancel the callback to call when the user cancels
    /// @param onChange the callback to call when the color changes at all
    /// @return the created color picker object
    BSML_EXPORT BSML::ColorSetting* CreateColorPicker(const TransformWrapper& parent, StringW label, UnityEngine::Color defaultColor, std::function<void(UnityEngine::Color)> onDone = nullptr, std::function<void()> onCancel = nullptr, std::function<void(UnityEngine::Color)> onChange = nullptr);

    /// @brief Creates a color picker modal
    /// @param parent what to parent it to
    /// @param name the text to display
    /// @param defaultColor the color to start off with
    /// @param onDone the callback to call when color picking is done
    /// @param onCancel the callback to call when the user cancels
    /// @param onChange the callback to call when the color changes at all
    /// @return the created color picker modal object
    BSML_EXPORT BSML::ModalColorPicker* CreateColorPickerModal(const TransformWrapper& parent, StringW name, UnityEngine::Color defaultColor, std::function<void(UnityEngine::Color)> onDone = nullptr, std::function<void()> onCancel = nullptr, std::function<void(UnityEngine::Color)> onChange = nullptr);

    /// @brief creates a toggle to turn things off / on
    /// @param parent what to parent it to
    /// @param text the label to give to the toggle
    /// @param currentValue the current value of the toggle
    /// @param anchoredPosition the position of the toggle
    /// @param onToggle what to do when the toggle is clicked
    /// @return the created toggle
    BSML_EXPORT BSML::ToggleSetting* CreateToggle(const TransformWrapper& parent, StringW text, bool currentValue, UnityEngine::Vector2 anchoredPosition, std::function<void(bool)> onToggle = nullptr);

    /// @brief creates a toggle to turn things off / on
    /// @param parent what to parent it to
    /// @param text the label to give to the toggle
    /// @param currentValue the current value of the toggle
    /// @param onToggle what to do when the toggle is clicked
    /// @return the created toggle
    inline BSML::ToggleSetting* CreateToggle(const TransformWrapper& parent, StringW text, bool currentValue, std::function<void(bool)> onToggle = nullptr) {
        return CreateToggle(parent, text, currentValue, {0, 0}, onToggle);
    }

    /// @brief creates a toggle to turn things off / on
    /// @param parent what to parent it to
    /// @param text the label to give to the toggle
    /// @param anchoredPosition the position of the toggle
    /// @param onToggle what to do when the toggle is clicked
    /// @return the created toggle
    inline BSML::ToggleSetting* CreateToggle(const TransformWrapper& parent, StringW text, UnityEngine::Vector2 anchoredPosition, std::function<void(bool)> onToggle = nullptr) {
        return CreateToggle(parent, text, false, anchoredPosition, onToggle);
    }

    /// @brief creates a toggle to turn things off / on
    /// @param parent what to parent it to
    /// @param text the label to give to the toggle
    /// @param onToggle what to do when the toggle is clicked
    /// @return the created toggle
    inline BSML::ToggleSetting* CreateToggle(const TransformWrapper& parent, StringW text, std::function<void(bool)> onToggle = nullptr) {
        return CreateToggle(parent, text, false, {0, 0}, onToggle);
    }
}
