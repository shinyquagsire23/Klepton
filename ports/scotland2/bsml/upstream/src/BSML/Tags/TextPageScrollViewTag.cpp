#include "BSML/Tags/TextPageScrollViewTag.hpp"
#include "Helpers/getters.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "BSML/Components/TextPageScrollViewRefresher.hpp"

#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/TextPageScrollView.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "GlobalNamespace/EulaDisplayViewController.hpp"

namespace BSML {
    static BSMLNodeParser<TextPageScrollViewTag> textPageScrollViewTagParser({"text-page", "page"});

    HMUI::TextPageScrollView* get_textPageTemplate() {
        static SafePtrUnity<HMUI::TextPageScrollView> textPageTemplate;
        if (!textPageTemplate) {
            textPageTemplate = UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::EulaDisplayViewController*>()->First()->_textPageScrollView;
        }
        return textPageTemplate.ptr();
    }

    UnityEngine::GameObject* TextPageScrollViewTag::CreateObject(UnityEngine::Transform* parent) const {

        HMUI::TextPageScrollView* scrollView = UnityEngine::Object::Instantiate(get_textPageTemplate(), parent);

        scrollView->set_name("BSMLTextScrollPageView");
        scrollView->set_enabled(true);

        scrollView->_platformHelper = Helpers::GetIVRPlatformHelper();

        TMPro::TextMeshProUGUI* textMesh = scrollView->_text;
        textMesh->set_text("Default Text");

        textMesh->get_gameObject()->AddComponent<BSML::TextPageScrollViewRefresher*>()->scrollView = scrollView;

        auto externalComponents = scrollView->get_gameObject()->AddComponent<ExternalComponents*>();

        externalComponents->Add(textMesh);

        return scrollView->get_gameObject();
    }
}
