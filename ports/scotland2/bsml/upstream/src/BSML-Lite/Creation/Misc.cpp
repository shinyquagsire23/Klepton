#include "BSML-Lite/Creation/Misc.hpp"

#include "Helpers/getters.hpp"
#include "Helpers/creation.hpp"

#define protected public
#include "BSML/Tags/TextSegmentedControlTag.hpp"
#undef protected

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/UI/CanvasScaler.hpp"
#include "UnityEngine/AdditionalCanvasShaderChannels.hpp"
#include "System/Action_2.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"

namespace BSML::Lite {
    HMUI::HoverHint* AddHoverHint(const GameObjectWrapper& parent, StringW text) {
        return BSML::Helpers::AddHoverHint(parent, text);
    }

    BSML::FloatingScreen* CreateFloatingScreen(UnityEngine::Vector2 screenSize, UnityEngine::Vector3 position, UnityEngine::Vector3 rotation, float curvatureRadius, bool hasBackground, bool createHandle, BSML::Side handleSide) {
        auto screen = BSML::FloatingScreen::CreateFloatingScreen(screenSize, createHandle, position, UnityEngine::Quaternion::Euler(rotation), curvatureRadius, hasBackground);
        screen->set_HandleSide(handleSide);

        return screen;
    }

    BSML::ProgressBar* CreateProgressBar(UnityEngine::Vector3 position, UnityEngine::Vector3 rotation, UnityEngine::Vector3 scale, StringW headerText, StringW subText1, StringW subText2) {
        return BSML::ProgressBar::CreateProgressBar(position, scale, rotation, headerText, subText1, subText2);
    }

    HMUI::TextSegmentedControl* CreateTextSegmentedControl(const TransformWrapper& parent, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta, std::span<std::string_view> values, std::function<void(int)> onCellWithIdxClicked) {
        auto go = BSML::TextSegmentedControlTag{}.CreateObject(parent);
        auto textSegmentedControl = go->GetComponent<HMUI::TextSegmentedControl*>();

        auto rect = go->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);
        rect->set_sizeDelta(sizeDelta);

        auto texts = ListW<StringW>::New();
        texts->EnsureCapacity(values.size());
        for (const auto& text : values) texts->Add(text);
        textSegmentedControl->SetTexts(*texts, nullptr);

        if (onCellWithIdxClicked) {
            textSegmentedControl->add_didSelectCellEvent(
                custom_types::MakeDelegate<System::Action_2<UnityW<HMUI::SegmentedControl>, int>*>(
                    std::function<void(HMUI::SegmentedControl*, int)>( [onCellWithIdxClicked](HMUI::SegmentedControl* _, int idx) { onCellWithIdxClicked(idx); } )
                )
            );
        }

        return textSegmentedControl;
    }

    static inline UnityEngine::AdditionalCanvasShaderChannels operator |(UnityEngine::AdditionalCanvasShaderChannels a, UnityEngine::AdditionalCanvasShaderChannels b) {
        return UnityEngine::AdditionalCanvasShaderChannels(a.value__ | b.value__);
    }

    UnityEngine::GameObject* CreateCanvas() {
        static ConstString name("BSMLCanvas");
        auto go = UnityEngine::GameObject::New_ctor(name);
        go->set_layer(5);
        auto cv = go->AddComponent<UnityEngine::Canvas*>();
        cv->set_additionalShaderChannels(UnityEngine::AdditionalCanvasShaderChannels::TexCoord1 | UnityEngine::AdditionalCanvasShaderChannels::TexCoord2);
        cv->set_sortingOrder(4);

        auto scaler = go->AddComponent<UnityEngine::UI::CanvasScaler*>();
        scaler->set_scaleFactor(1.0f);
        scaler->set_dynamicPixelsPerUnit(3.44f);
        scaler->set_referencePixelsPerUnit(10.0f);

        auto* physicsRaycaster = Helpers::GetPhysicsRaycasterWithCache();
        if(physicsRaycaster)
            go->AddComponent<VRUIControls::VRGraphicRaycaster*>()->_physicsRaycaster = physicsRaycaster;

        auto rectTransform = go->GetComponent<UnityEngine::RectTransform*>();
        float scale = 1.5f * 0.02f; //Wrapper->ScreenSystem: 1.5 Wrapper->ScreenSystem->ScreenContainer: 0.02
        rectTransform->set_localScale(UnityEngine::Vector3(scale, scale, scale));
        return go;
    }
}
