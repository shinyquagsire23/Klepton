#include "BSML/Tags/Settings/SubmenuTag.hpp"
#include "BSML.hpp"
#include "BSML/Settings/UI/ModSettingsFlowCoordinator.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/creation.hpp"
#include "BSML/Components/ClickableText.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "BSML/Settings/SettingsMenu.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Resources.hpp"

using HapticPresetSO = Libraries::HM::HMLib::VR::HapticPresetSO;

namespace BSML {
    
    static BSMLNodeParser<SubmenuTag> submenuTagParser({"settings-submenu"});

    extern GlobalNamespace::Signal* get_textClickedSignal();
    extern HapticPresetSO* get_textHapticPreset();
    extern GlobalNamespace::HapticFeedbackManager* get_textHapticFeedbackManager();

    ModSettingsFlowCoordinator* get_flow() {
        static SafePtrUnity<ModSettingsFlowCoordinator> flow;
        if (!flow) {
            flow = UnityEngine::Resources::FindObjectsOfTypeAll<ModSettingsFlowCoordinator*>()->FirstOrDefault();
        }
        return flow.ptr();
    }

    UnityEngine::GameObject* SubmenuTag::CreateObject(UnityEngine::Transform* parent) const {
        auto go = UnityEngine::GameObject::New_ctor("BSMLSubMenu");
        go->SetActive(false);

        ClickableText* clickableText = go->AddComponent<ClickableText*>();
        clickableText->set_font(Helpers::GetMainTextFont());
        clickableText->set_fontSharedMaterial(Helpers::GetMainUIFontMaterial());
        clickableText->get_rectTransform()->SetParent(parent, false);
        clickableText->set_text("BSMLText");
        clickableText->set_fontSize(4);
        clickableText->set_fontStyle(TMPro::FontStyles::Italic);
        clickableText->set_color({1, 1, 1, 1});
        clickableText->get_rectTransform()->set_sizeDelta({90, 8});

        HMUI::ViewController* submenuController = Helpers::CreateViewController<HMUI::ViewController*>();
        SettingsMenu::SetupViewControllerTransform(submenuController);

        clickableText->onClick += [submenuController](){
            auto flow = get_flow();
            if (flow) {
                flow->OpenMenu(submenuController, true, false);
            }
        };

        auto externalComponents = submenuController->get_gameObject()->AddComponent<ExternalComponents*>();
        externalComponents->Add(clickableText);
        externalComponents->Add(clickableText->get_rectTransform());

        return submenuController->get_gameObject();
    }
}
