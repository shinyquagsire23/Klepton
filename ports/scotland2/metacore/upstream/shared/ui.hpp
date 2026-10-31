#pragma once

#if __has_include("bsml/shared/BSML-Lite.hpp")

#include "HMUI/IconSegmentedControl.hpp"
#include "HMUI/InputFieldView.hpp"
#include "HMUI/ModalView.hpp"
#include "HMUI/TextSegmentedControl.hpp"
#include "bsml/shared/BSML-Lite/TransformWrapper.hpp"
#include "bsml/shared/BSML/Components/Settings/DropdownListSetting.hpp"
#include "bsml/shared/BSML/Components/Settings/IncrementSetting.hpp"
#include "bsml/shared/BSML/Components/Settings/SliderSetting.hpp"
#include "bsml/shared/BSML/Components/Settings/ToggleSetting.hpp"

namespace MetaCore::UI {
    /// @brief Sets the preferred size parameters of the LayoutElement on a given object, adding one if not present
    /// @param object The object with a LayoutElement or to have one added
    /// @param width The preferred width for the LayoutElement (-1 is unset)
    /// @param height The preferred height for the LayoutElement (-1 is unset)
    /// @param flexWidth The flexible width for the LayoutElement (-1 is unset)
    /// @param flexHeight The flexible height for the LayoutElement (-1 is unset)
    inline void SetLayoutSize(BSML::Lite::TransformWrapper object, float width, float height, float flexWidth = -1, float flexHeight = -1);
    /// @brief Sets the preferred width of all LayoutElements in the immediate children of a given object
    /// @param parent The parent object
    /// @param width The preferred width for parent's children
    inline void SetChildrenWidth(BSML::Lite::TransformWrapper parent, float width);
    /// @brief Sets the sorting order of a Canvas, enabling overrideSorting and temporarily toggling it active if necessary
    /// @param canvas The object with a Canvas
    /// @param value The new sorting order
    inline void SetCanvasSorting(BSML::Lite::TransformWrapper canvas, int value);

    /// @brief Sets the value of an toggle setting without playing the animation, does not trigger its change callback
    /// @param toggle The toggle setting to modify
    /// @param value The new value
    inline void InstantSetToggle(BSML::ToggleSetting* toggle, bool value);
    /// @brief Sets the value of an increment setting, does not trigger its change callback
    /// @param increment The increment setting to modify
    /// @param value The new value
    inline void SetIncrementValue(BSML::IncrementSetting* increment, float value);
    /// @brief Sets the selected value for a dropdown setting, does not trigger its selection callback
    /// @param dropdown The dropdown setting to modify
    /// @param value The new value to have selected
    /// @return If the value was in the possible options and was selected
    inline bool SetDropdownValue(BSML::DropdownListSetting* dropdown, std::string value);
    /// @brief Sets the options and selected value for a dropdown setting, does not trigger its selection callback
    /// @param dropdown The dropdown setting to modify
    /// @param values The new value options for the dropdown
    /// @param selected The option that should be selected
    /// @return If the option that should be selected was in the list of values, if not the first value will be selected
    inline bool SetDropdownValues(BSML::DropdownListSetting* dropdown, std::vector<std::string> values, std::string selected);
    /// @brief Sets a cell of an IconSegmentedControl to be interactable or not
    /// @param cell The cell to change
    /// @param interactable If the cell should be interactable
    inline void SetIconSegmentInteractable(HMUI::IconSegmentedControl* iconControl, int cell, bool interactable);
    /// @brief Sets the sprite of an icon button (not a clickable image)
    /// @param button The button to modify, previously created with CreateIconButton
    /// @param sprite The new sprite for the button
    inline void SetIconButtonSprite(UnityEngine::UI::Button* button, UnityEngine::Sprite* sprite);
    /// @brief Resizes and adds an image to a BSML-created button
    /// @param button The button to modify
    /// @param sprite The sprite for the button
    inline void ConvertToIconButton(UnityEngine::UI::Button* button, UnityEngine::Sprite* sprite);

    /// @brief Creates extra increment buttons around an increment setting, shifting the increment to the left to compensate
    /// @param setting The increment setting to modify
    /// @param increment The amount to increase or decrease the setting by when the new buttons are pressed
    inline void AddIncrementIncrement(BSML::IncrementSetting* setting, float increment);

    /// @brief Adds or sets a callback for when a specific slider is released (both by dragging or a single press)
    /// @param slider The slider to monitor
    /// @param onEndDrag The callback to run when released
    inline void AddSliderEndDrag(BSML::SliderSetting* slider, std::function<void(float)> onEndDrag);

    /// @brief Adds or sets a callback for when a specific string setting's keyboard closes, either by clicking away, pressing the OK button
    /// @param input The string setting to monitor
    /// @param onClosed The callback to run when the keyboard closes or the clear button is pressed
    inline void AddStringSettingOnClose(HMUI::InputFieldView* input, std::function<void()> onClosed);
    /// @brief Adds or sets callbacks for when a specific string setting's keyboard is closed, and for when its OK button is pressed
    /// @param input The string setting to monitor
    /// @param onClosed The callback to run when the keyboard is closed by clicking away, or the clear button is pressed if onOk is null
    /// @param onOk The callback to run when the OK button is pressed, or the clear button is pressed
    inline void AddStringSettingOnClose(HMUI::InputFieldView* input, std::function<void()> onClosed, std::function<void()> onOk);

    /// @brief Animates a modal to fade in or out so that modals on top of it have clear boundaries
    /// @param modal The modal to animate
    /// @param out If the modal should fade out and become slightly transparent
    inline void AnimateModal(HMUI::ModalView* modal, bool out);
    /// @brief Adds animations to a dropdown such that the modal behind it fades out when the dropdown modal is opened
    /// @param dropdown The dropdown to trigger the animations
    /// @param behindModal The modal to fade out behind the dropdown modal
    inline void AddModalAnimations(HMUI::SimpleTextDropdown* dropdown, HMUI::ModalView* behindModal);

    /// @brief Moves a slider created with BSML to have a different parent, and removes its label text
    /// @param slider The slider to move
    /// @param parent The new parent for the slider
    /// @param width The new width for the slider area
    /// @return The new slider component
    inline BSML::SliderSetting* ReparentSlider(BSML::SliderSetting* slider, BSML::Lite::TransformWrapper parent, float width);

    /// @brief Creates a square button with an image inside instead of text
    /// @param parent The parent for the button
    /// @param sprite The sprite to display inside the button
    /// @param onClick The callback when the button is clicked
    /// @return The created button
    inline UnityEngine::UI::Button*
    CreateIconButton(BSML::Lite::TransformWrapper parent, UnityEngine::Sprite* sprite, std::function<void()> onClick = nullptr);
    /// @brief Creates a square button with an image inside instead of text, with a non-default button template
    /// @param parent The parent for the button
    /// @param sprite The sprite to display inside the button
    /// @param buttonTemplate The instantiated template to use for the button
    /// @param onClick The callback when the button is clicked
    /// @return The created button
    inline UnityEngine::UI::Button* CreateIconButton(
        BSML::Lite::TransformWrapper parent, UnityEngine::Sprite* sprite, std::string buttonTemplate, std::function<void()> onClick = nullptr
    );

    /// @brief Creates a dropdown setting for strings, with a fixed height
    /// @param parent The parent for the dropdown
    /// @param name The setting name to display next to the dropdown
    /// @param value The starting value of the dropdown
    /// @param values The list of possible values for the dropdown
    /// @param onChange The callback when a dropdown option is selected (by displayed name)
    /// @return The created dropdown setting
    inline BSML::DropdownListSetting* CreateDropdown(
        BSML::Lite::TransformWrapper parent,
        std::string name,
        std::string value,
        std::vector<std::string_view> values,
        std::function<void(std::string)> onChange
    );
    /// @brief Creates a dropdown setting for an enum, with a fixed height
    /// @param parent The parent for the dropdown
    /// @param name The setting name to display next to the dropdown
    /// @param value The starting value of the dropdown
    /// @param values The list of possible values for the dropdown
    /// @param onChange The callback when a dropdown option is selected (by index)
    /// @return The created dropdown setting
    inline BSML::DropdownListSetting* CreateDropdownEnum(
        BSML::Lite::TransformWrapper parent, std::string name, int value, std::vector<std::string_view> values, std::function<void(int)> onChange
    );
    /// @brief Creates a TextSegmentedControl like in the help menu
    /// @param parent The parent for the TextSegmentedControl
    /// @param texts The texts to display in each segment
    /// @param onSelected The callback when a segment is selected (by index)
    /// @return The created TextSegmentedControl
    inline HMUI::TextSegmentedControl*
    CreateTextSegmentedControl(BSML::Lite::TransformWrapper parent, std::vector<std::string_view> texts, std::function<void(int)> onSelected);
    /// @brief Creates an IconSegmentedControl like the Custom Levels/Favorites/etc selector
    /// @param parent The parent for the IconSegmentedControl
    /// @param icons The icons to display in each segment
    /// @param onSelected The callback when a segment is selected (by index)
    /// @param hints The hover hints to display for each segment
    /// @return The created IconSegmentedControl
    inline HMUI::IconSegmentedControl* CreateIconSegmentedControl(
        BSML::Lite::TransformWrapper parent,
        std::vector<UnityEngine::Sprite*> icons,
        std::function<void(int)> onSelected,
        std::vector<std::string> hints = {}
    );
}

#include "uiimpl.hpp"

#endif
