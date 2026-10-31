#include "BSML/Tags/Settings/GenericSliderSettingTag.hpp"
#include "logging.hpp"

#include "BSML/Components/Settings/SliderSetting.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "Helpers/getters.hpp"

#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/ColorBlock.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Color.hpp"
#include "HMUI/CustomFormatRangeValuesSlider.hpp"
#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"
#include "GlobalNamespace/MainSettingsMenuViewController.hpp"
#include "BeatSaber/GameSettings/ControllerProfilesSettingsViewController.hpp"
#include "GlobalNamespace/SettingsSubMenuInfo.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    LayoutElement* get_controllersTransformTemplate() {
        static SafePtrUnity<LayoutElement> controllersTransformTemplate;
        if (!controllersTransformTemplate) {
            controllersTransformTemplate = Helpers::GetDiContainer()->Resolve<GlobalNamespace::MainSettingsMenuViewController*>()->
                _settingsSubMenuInfos->First([](auto x){
                    return il2cpp_utils::try_cast<BeatSaber::GameSettings::ControllerProfilesSettingsViewController>(x->viewController.ptr());
                })->viewController->transform->
                Find("Content/MainContent/Sliders/PositionX")->
                GetComponent<LayoutElement*>();
            if (!controllersTransformTemplate) {
                ERROR("No controllersTransformTemplate found!");
                return nullptr;
            }
        }
        return controllersTransformTemplate.ptr();
    }

    UnityEngine::GameObject* GenericSliderSettingTagBase::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating SliderSettingBase");
        auto baseSetting = Object::Instantiate(get_controllersTransformTemplate(), parent, false);
        auto gameObject = baseSetting->get_gameObject();
        gameObject->set_name("BSMLSliderSetting");

        auto rectTransform = baseSetting->transform.cast<RectTransform>();
        rectTransform->set_anchoredPosition({0, 0});
        Object::Destroy(rectTransform->Find("SliderLeft")->gameObject);
        Object::Destroy(baseSetting->GetComponent<CanvasGroup*>());

        auto sliderSetting = gameObject->AddComponent(get_type()).cast<BSML::SliderSettingBase>();
        auto slider = gameObject->GetComponentInChildren<HMUI::CustomFormatRangeValuesSlider*>();
        sliderSetting->slider = slider;

        // colors to not be red
        auto& colorBlock = slider->___m_Colors;
        colorBlock.set_normalColor({0, 0, 0, 0.5});
        colorBlock.set_highlightedColor({1, 1, 1, 0.2});
        colorBlock.set_pressedColor({1, 1, 1, 0.2});
        colorBlock.set_selectedColor({1, 1, 1, 0.2});
        colorBlock.set_disabledColor({0.8, 0.8, 0.8, 0.5});

        slider->set_name("BSMLSlider");
        slider->GetComponentInChildren<TMPro::TextMeshProUGUI*>()->set_enableWordWrapping(false);
        slider->_enableDragging = true;
        auto sliderRect = slider->transform.cast<RectTransform>();
        sliderRect->set_anchorMin({1, 0});
        sliderRect->set_anchorMax({1, 1});
        sliderRect->set_sizeDelta({52, 0});
        sliderRect->set_pivot({1, 0.5f});
        sliderRect->set_anchoredPosition({0, 0});

        auto nameText = gameObject->get_transform()->Find("Title")->get_gameObject();
        Object::Destroy(nameText->GetComponent<BGLib::Polyglot::LocalizedTextMeshProUGUI*>());

        auto titleTransform = nameText->get_transform().cast<RectTransform>();
        titleTransform->set_anchorMin({0, 0});
        titleTransform->set_anchorMax({0, 0});
        titleTransform->set_offsetMin({0, 0});
        titleTransform->set_offsetMax({-52, 0});

        TMPro::TextMeshProUGUI* text = nameText->GetComponent<TMPro::TextMeshProUGUI*>();
        text->set_richText(true);
        text->set_text("BSMLSlider");
        text->get_rectTransform()->set_anchorMax({1, 1});
        text->set_alignment(::TMPro::TextAlignmentOptions::CaplineLeft);
        text->set_enableWordWrapping(false);
        text->set_overflowMode(::TMPro::TextOverflowModes::Overflow);

        baseSetting->set_preferredWidth(90.0f);

        auto externalComponents = gameObject->AddComponent<ExternalComponents*>();
        externalComponents->Add(text);

        return gameObject;
    }
}
