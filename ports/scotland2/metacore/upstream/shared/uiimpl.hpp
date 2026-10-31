#pragma once

#include "HMUI/AnimatedSwitchView.hpp"
#include "HMUI/ButtonBinder.hpp"
#include "HMUI/IconSegmentedControlCell.hpp"
#include "HMUI/ModalView.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/CanvasGroup.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "bsml/shared/BSML-Lite.hpp"
#include "delegates.hpp"
#include "internals.hpp"
#include "ui.hpp"
#include "unity.hpp"

#define UUI UnityEngine::UI

inline void MetaCore::UI::SetLayoutSize(BSML::Lite::TransformWrapper object, float width, float height, float flexWidth, float flexHeight) {
    auto layout = Engine::GetOrAddComponent<UUI::LayoutElement*>(object);
    layout->preferredWidth = width;
    layout->preferredHeight = height;
    layout->flexibleWidth = flexWidth;
    layout->flexibleHeight = flexHeight;
}

inline void MetaCore::UI::SetChildrenWidth(BSML::Lite::TransformWrapper parent, float width) {
    for (int i = 0; i < parent->GetChildCount(); i++) {
        bool first = true;
        for (auto layout : parent->GetChild(i)->GetComponents<UUI::LayoutElement*>()) {
            if (first)
                layout->preferredWidth = width;
            else
                UnityEngine::Object::Destroy(layout);
            first = false;
        }
    }
}

inline void MetaCore::UI::SetCanvasSorting(BSML::Lite::TransformWrapper canvas, int value) {
    auto object = canvas->gameObject;
    bool wasActive = object->activeSelf;
    object->active = true;
    auto component = object->GetComponent<UnityEngine::Canvas*>();
    component->overrideSorting = true;
    component->sortingOrder = value;
    object->active = wasActive;
}

inline void MetaCore::UI::InstantSetToggle(BSML::ToggleSetting* toggle, bool value) {
    if (toggle->toggle->m_IsOn == value)
        return;
    toggle->toggle->m_IsOn = value;
    auto animatedSwitch = toggle->toggle->GetComponent<HMUI::AnimatedSwitchView*>();
    animatedSwitch->HandleOnValueChanged(value);
    animatedSwitch->_switchAmount = value;
    animatedSwitch->LerpPosition(value);
    animatedSwitch->LerpColors(value, animatedSwitch->_highlightAmount, animatedSwitch->_disabledAmount);
}

inline void MetaCore::UI::SetIncrementValue(BSML::IncrementSetting* increment, float value) {
    increment->currentValue = value;
    increment->UpdateState();
}

inline bool MetaCore::UI::SetDropdownValue(BSML::DropdownListSetting* dropdown, std::string value) {
    auto values = ListW<StringW>(dropdown->values);
    for (int i = 0; i < values.size(); i++) {
        if (values[i] == value) {
            dropdown->set_Value(dropdown->values[i]);
            return true;
        }
    }
    return false;
}

inline bool MetaCore::UI::SetDropdownValues(BSML::DropdownListSetting* dropdown, std::vector<std::string> values, std::string selected) {
    auto texts = ListW<System::Object*>::New(values.size());
    int idx = -1;
    bool present = true;
    for (int i = 0; i < values.size(); i++) {
        texts->Add((System::Object*) StringW(values[i]).convert());
        if (values[i] == selected)
            idx = i;
    }
    if (idx == -1) {
        idx = 0;
        present = false;
    }
    dropdown->values = texts;
    dropdown->UpdateChoices();
    if (!texts.empty())
        dropdown->set_Value(texts[idx]);
    return present;
}

inline void MetaCore::UI::SetIconSegmentInteractable(HMUI::IconSegmentedControl* iconControl, int cell, bool interactable) {
    iconControl->_dataItems[cell]->interactable = false;
    auto cellInstance = iconControl->_cells->get_Item(cell).cast<HMUI::IconSegmentedControlCell>();
    cellInstance->hideBackgroundImage = !interactable;
    cellInstance->enabled = interactable;
    cellInstance->SetHighlight(false, HMUI::SelectableCell::TransitionType::Instant, true);
}

inline void MetaCore::UI::SetIconButtonSprite(UUI::Button* button, UnityEngine::Sprite* sprite) {
    if (auto icon = button->transform->Find("MetaCoreButtonImage"))
        icon->GetComponent<HMUI::ImageView*>()->sprite = sprite;
}

inline void MetaCore::UI::ConvertToIconButton(UUI::Button* button, UnityEngine::Sprite* sprite) {
    auto icon = BSML::Lite::CreateImage(button, sprite);
    icon->name = "MetaCoreButtonImage";
    icon->preserveAspect = true;
    icon->transform->localScale = {0.8, 0.8, 0.8};
    MetaCore::UI::SetLayoutSize(button, 8, 8);
}

inline void MetaCore::UI::AddIncrementIncrement(BSML::IncrementSetting* setting, float increment) {
    auto transform = setting->transform->Find("ValuePicker").cast<UnityEngine::RectTransform>();
    auto currentPos = transform->anchoredPosition;
    transform->anchoredPosition = {currentPos.x - 6, currentPos.y};

    auto leftButton = BSML::Lite::CreateUIButton(transform, "", "DecButton", {-7, 0}, {7, 8}, [setting, increment]() {
        setting->currentValue -= increment;
        setting->EitherPressed();
    });
    auto rightButton = BSML::Lite::CreateUIButton(transform, "", "IncButton", {7, 0}, {8, 8}, [setting, increment]() {
        setting->currentValue += increment;
        setting->EitherPressed();
    });
}

inline void MetaCore::UI::AddSliderEndDrag(BSML::SliderSetting* slider, std::function<void(float)> onEndDrag) {
    std::function<void()> boundCallback = [slider = slider->slider, onEndDrag]() {
        onEndDrag(slider->value);
    };
    Internals::SetEndDragUI(slider->slider, boundCallback);
    if (slider->showButtons && slider->incButton && slider->decButton) {
        slider->incButton->onClick->AddListener(Delegates::MakeUnityAction(boundCallback));
        slider->decButton->onClick->AddListener(Delegates::MakeUnityAction(boundCallback));
    }
}

inline void MetaCore::UI::AddStringSettingOnClose(HMUI::InputFieldView* input, std::function<void()> onClosed) {
    AddStringSettingOnClose(input, onClosed, onClosed);
}

inline void MetaCore::UI::AddStringSettingOnClose(HMUI::InputFieldView* input, std::function<void()> onClosed, std::function<void()> onOk) {
    auto clearCallback = Internals::SetKeyboardCloseUI(input, std::move(onClosed), std::move(onOk));
    input->_buttonBinder->AddBinding(input->_clearSearchButton, Delegates::MakeSystemAction(clearCallback));
}

inline void MetaCore::UI::AnimateModal(HMUI::ModalView* modal, bool out) {
    auto bg = modal->transform->Find("BG")->GetComponent<UUI::Image*>();
    auto canvas = modal->GetComponent<UnityEngine::CanvasGroup*>();

    if (out) {
        bg->color = {0.2, 0.2, 0.2, 1};
        canvas->alpha = 0.9;
    } else {
        bg->color = {1, 1, 1, 1};
        canvas->alpha = 1;
    }
}

inline void MetaCore::UI::AddModalAnimations(HMUI::SimpleTextDropdown* dropdown, HMUI::ModalView* behindModal) {
    dropdown->_button->onClick->AddListener(Delegates::MakeUnityAction([behindModal]() { AnimateModal(behindModal, true); }));
    dropdown->add_didSelectCellWithIdxEvent(Delegates::MakeSystemAction([behindModal](UnityW<HMUI::DropdownWithTableView>, int) {
        AnimateModal(behindModal, false);
    }));
    dropdown->_modalView->add_blockerClickedEvent(Delegates::MakeSystemAction([behindModal]() { AnimateModal(behindModal, false); }));
    if (auto modal = il2cpp_utils::try_cast<HMUI::ModalView>(dropdown->_modalView.unsafePtr()).value_or(nullptr))
        modal->_animateParentCanvas = false;
}

inline BSML::SliderSetting* MetaCore::UI::ReparentSlider(BSML::SliderSetting* slider, BSML::Lite::TransformWrapper parent, float width) {
    auto newSlider = slider->slider->gameObject->AddComponent<BSML::SliderSetting*>();
    newSlider->slider = slider->slider;
    newSlider->onChange = std::move(slider->onChange);
    newSlider->formatter = std::move(slider->formatter);
    newSlider->isInt = slider->isInt;
    newSlider->increments = slider->increments;
    newSlider->slider->minValue = slider->slider->minValue;
    newSlider->slider->maxValue = slider->slider->maxValue;
    auto transform = newSlider->GetComponent<UnityEngine::RectTransform*>();
    transform->sizeDelta = {width, 0};
    transform->SetParent(parent->transform, false);
    newSlider->slider->valueSize = newSlider->slider->_containerRect->rect.width / 2;
    UnityEngine::Object::DestroyImmediate(slider->gameObject);
    // due to the weird way bsml does string formatting for sliders,
    // this needs to be called after destroying the old slider
    newSlider->Setup();
    return newSlider;
}

inline UUI::Button* MetaCore::UI::CreateIconButton(BSML::Lite::TransformWrapper parent, UnityEngine::Sprite* sprite, std::function<void()> onClick) {
    auto button = BSML::Lite::CreateUIButton(parent, "", onClick);
    ConvertToIconButton(button, sprite);
    return button;
}

inline UUI::Button* MetaCore::UI::CreateIconButton(
    BSML::Lite::TransformWrapper parent, UnityEngine::Sprite* sprite, std::string buttonTemplate, std::function<void()> onClick
) {
    auto button = BSML::Lite::CreateUIButton(parent, "", buttonTemplate, onClick);
    ConvertToIconButton(button, sprite);
    return button;
}

inline BSML::DropdownListSetting* MetaCore::UI::CreateDropdown(
    BSML::Lite::TransformWrapper parent,
    std::string name,
    std::string value,
    std::vector<std::string_view> values,
    std::function<void(std::string)> onChange
) {
    auto object = BSML::Lite::CreateDropdown(parent, name, value, values, [onChange](StringW value) { onChange(value); });
    object->transform->parent->GetComponent<UUI::LayoutElement*>()->preferredHeight = 7;
    return object;
}

inline BSML::DropdownListSetting* MetaCore::UI::CreateDropdownEnum(
    BSML::Lite::TransformWrapper parent, std::string name, int value, std::vector<std::string_view> values, std::function<void(int)> onChange
) {
    auto object = BSML::Lite::CreateDropdown(parent, name, values[value], values, [onChange, values](StringW value) {
        for (int i = 0; i < values.size(); i++) {
            if (value == values[i]) {
                onChange(i);
                break;
            }
        }
    });
    object->transform->parent->GetComponent<UUI::LayoutElement*>()->preferredHeight = 7;
    if (auto behindModal = parent->gameObject->GetComponentInParent<HMUI::ModalView*>(true))
        AddModalAnimations(object->dropdown, behindModal);
    return object;
}

inline HMUI::TextSegmentedControl* MetaCore::UI::CreateTextSegmentedControl(
    BSML::Lite::TransformWrapper parent, std::vector<std::string_view> texts, std::function<void(int)> onSelected
) {
    static UnityW<HMUI::TextSegmentedControl> textSegmentedControlTemplate;
    if (!textSegmentedControlTemplate) {
        textSegmentedControlTemplate = UnityEngine::Resources::FindObjectsOfTypeAll<HMUI::TextSegmentedControl*>()->FirstOrDefault([](auto x) {
            UnityEngine::Transform* parent = x->transform->parent;
            if (!parent)
                return false;
            if (parent->name != "PlayerStatisticsViewController")
                return false;
            return x->_container != nullptr;
        });
    }

    auto textSegmentedControl = UnityEngine::Object::Instantiate(textSegmentedControlTemplate.ptr(), parent->transform, false);
    textSegmentedControl->_dataSource = nullptr;

    UnityEngine::GameObject* gameObject = textSegmentedControl->gameObject;
    gameObject->name = "MetaCoreTextSegmentedControl";
    textSegmentedControl->_container = textSegmentedControlTemplate->_container;

    auto transform = gameObject->GetComponent<UnityEngine::RectTransform*>();
    transform->anchoredPosition = {0, 0};
    int childCount = transform->childCount;
    for (int i = 1; i <= childCount; i++)
        UnityEngine::Object::DestroyImmediate(transform->GetChild(childCount - i)->gameObject);

    auto textList = ListW<StringW>::New(texts.size());
    for (auto text : texts)
        textList->Add(text);
    textSegmentedControl->SetTexts(textList->i___System__Collections__Generic__IReadOnlyList_1_T_(), nullptr);

    if (onSelected)
        textSegmentedControl->add_didSelectCellEvent(Delegates::MakeSystemAction([onSelected](UnityW<HMUI::SegmentedControl>, int selectedIndex) {
            onSelected(selectedIndex);
        }));

    gameObject->active = true;
    return textSegmentedControl;
}

inline HMUI::IconSegmentedControl* MetaCore::UI::CreateIconSegmentedControl(
    BSML::Lite::TransformWrapper parent, std::vector<UnityEngine::Sprite*> icons, std::function<void(int)> onSelected, std::vector<std::string> hints
) {
    static UnityW<HMUI::IconSegmentedControl> iconSegmentedControlTemplate;
    if (!iconSegmentedControlTemplate) {
        iconSegmentedControlTemplate = UnityEngine::Resources::FindObjectsOfTypeAll<HMUI::IconSegmentedControl*>()->FirstOrDefault([](auto x) {
            if (std::string(x->name) != "BeatmapCharacteristicSegmentedControl")
                return false;
            return x->_container != nullptr;
        });
    }

    auto iconSegmentedControl = UnityEngine::Object::Instantiate(iconSegmentedControlTemplate.ptr(), parent->transform, false);
    iconSegmentedControl->_isInitialized = false;
    iconSegmentedControl->_dataSource = nullptr;
    iconSegmentedControl->_hideCellBackground = false;
    iconSegmentedControl->_overrideCellSize = true;
    iconSegmentedControl->_iconSize = 4;
    iconSegmentedControl->_padding = 1;

    UnityEngine::GameObject* gameObject = iconSegmentedControl->gameObject;
    gameObject->name = "MetaCoreIconSegmentedControl";
    iconSegmentedControl->_container = iconSegmentedControlTemplate->_container;

    auto transform = gameObject->GetComponent<UnityEngine::RectTransform*>();
    transform->anchoredPosition = {0, 0};
    int childCount = transform->childCount;
    for (int i = 1; i <= childCount; i++)
        UnityEngine::Object::DestroyImmediate(transform->GetChild(childCount - i)->gameObject);

    auto dataList = ListW<HMUI::IconSegmentedControl::DataItem*>::New(icons.size());
    for (int i = 0; i < icons.size(); i++)
        dataList->Add(HMUI::IconSegmentedControl::DataItem::New_ctor(icons[i], i < hints.size() ? hints[i] : "", true));
    iconSegmentedControl->SetData(dataList.to_array());

    if (onSelected)
        iconSegmentedControl->add_didSelectCellEvent(Delegates::MakeSystemAction([onSelected](UnityW<HMUI::SegmentedControl>, int selectedIndex) {
            onSelected(selectedIndex);
        }));

    gameObject->active = true;
    return iconSegmentedControl;
}

#undef UUI
