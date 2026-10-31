#include "BSML/Tags/ClickableTextTag.hpp"
#include "BSML/Components/ClickableText.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/creation.hpp"
#include "logging.hpp"

#include "HMUI/Touchable.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"

#include "GlobalNamespace/Signal.hpp"
#include "GlobalNamespace/MenuShockwave.hpp"
#include "GlobalNamespace/HapticFeedbackManager.hpp"
#include "Libraries/HM/HMLib/VR/HapticPresetSO.hpp"

using namespace UnityEngine;
using HapticPresetSO = Libraries::HM::HMLib::VR::HapticPresetSO;

namespace BSML {
    static BSMLNodeParser<ClickableTextTag> clickableTextTagParser({"clickable-text"});

    GlobalNamespace::Signal* get_textClickedSignal() {
        static SafePtrUnity<GlobalNamespace::Signal> textClickedSignal;
        if (!textClickedSignal) {
            auto menuShockWave = Resources::FindObjectsOfTypeAll<GlobalNamespace::MenuShockwave*>()->FirstOrDefault();
            textClickedSignal = menuShockWave ? menuShockWave->_buttonClickEvents->LastOrDefault() : nullptr;
        }
        return textClickedSignal.ptr();
    }

    HapticPresetSO* get_textHapticPreset() {
        static SafePtrUnity<HapticPresetSO> textHapticPreset;
        if (!textHapticPreset) {
            textHapticPreset = UnityEngine::ScriptableObject::CreateInstance<HapticPresetSO*>();
            textHapticPreset->_duration = 0.02f;
            textHapticPreset->_strength = 1.0f;
            textHapticPreset->_frequency = 0.2f;
            Object::DontDestroyOnLoad(textHapticPreset.ptr());
        }
        return textHapticPreset.ptr();
    }

    GlobalNamespace::HapticFeedbackManager* get_textHapticFeedbackManager() {
        static SafePtrUnity<GlobalNamespace::HapticFeedbackManager> textHapticFeedbackManager;
        if (!textHapticFeedbackManager) {
            textHapticFeedbackManager = UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::HapticFeedbackManager*>()->First();
        }
        return textHapticFeedbackManager.ptr();
    }

    UnityEngine::GameObject* ClickableTextTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Clickable text");
        auto gameObject = GameObject::New_ctor("BSMLClickableText");
        gameObject->SetActive(false);
        gameObject->get_transform()->SetParent(parent, false);

        auto textMesh = gameObject->AddComponent<ClickableText*>();
        textMesh->set_font(Helpers::GetMainTextFont());
        textMesh->set_fontSharedMaterial(Helpers::GetMainUIFontMaterial());
        textMesh->set_text("BSMLClickableText");
        textMesh->set_fontSize(4);
        textMesh->set_color({1.0f, 1.0f, 1.0f, 1.0f});
        textMesh->set_richText(true);

        auto rectTransform = textMesh->get_rectTransform();
        rectTransform->set_anchorMin({0.5f, 0.5f});
        rectTransform->set_anchorMax({0.5f, 0.5f});
        rectTransform->set_sizeDelta({90, 8});

        gameObject->AddComponent<UI::LayoutElement*>();

        textMesh->buttonClickedSignal = get_textClickedSignal();
        textMesh->hapticFeedbackPresetSO = get_textHapticPreset();
        textMesh->hapticFeedbackManager = get_textHapticFeedbackManager();

        gameObject->SetActive(true);
        return gameObject;
    }
}
