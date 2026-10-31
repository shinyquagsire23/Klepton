#include "BSML/Tags/ScrollViewTag.hpp"
#include "BSML/Components/ScrollView.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "BSML/Components/ScrollViewContent.hpp"

#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "UnityEngine/TextAnchor.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/RectTransform.hpp"

#include "HMUI/TextPageScrollView.hpp"
#include "HMUI/VerticalScrollIndicator.hpp"

#include "TMPro/TextMeshProUGUI.hpp"

#include "GlobalNamespace/EulaDisplayViewController.hpp"

#include "VRUIControls/VRGraphicRaycaster.hpp"

#include "Helpers/getters.hpp"

namespace BSML {
    static BSMLNodeParser<ScrollViewTag> scrollViewTagParser({"scroll-view"});
    HMUI::TextPageScrollView* get_scrollViewTemplate() {
        static SafePtrUnity<HMUI::TextPageScrollView> scrollViewTemplate;
        if (!scrollViewTemplate) {
            scrollViewTemplate =  UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::EulaDisplayViewController*>()->First()->_textPageScrollView;
        }
        return scrollViewTemplate.ptr();
    }

    UnityEngine::GameObject* ScrollViewTag::CreateObject(UnityEngine::Transform* parent) const {
        HMUI::TextPageScrollView* textScrollView = UnityEngine::Object::Instantiate(get_scrollViewTemplate(), parent);
        auto gameObject = textScrollView->gameObject;
        gameObject->SetActive(false);

        textScrollView->set_name("BSMLScrollView");
        auto pageUpButton = textScrollView->_pageUpButton;
        auto pageDownButton = textScrollView->_pageDownButton;
        auto verticalScrollIndicator = textScrollView->_verticalScrollIndicator;

        UnityEngine::RectTransform* viewport = textScrollView->_viewport;
        viewport->get_gameObject()->AddComponent<VRUIControls::VRGraphicRaycaster*>()->_physicsRaycaster = Helpers::GetPhysicsRaycasterWithCache();

        UnityEngine::Object::Destroy(textScrollView->_text->get_gameObject());
        UnityEngine::Object::Destroy(textScrollView);
        gameObject->SetActive(false);

        BSML::ScrollView* scrollView = gameObject->AddComponent<BSML::ScrollView*>();
        scrollView->_pageUpButton = pageUpButton;
        scrollView->_pageDownButton = pageDownButton;
        scrollView->_verticalScrollIndicator = verticalScrollIndicator;
        scrollView->_viewport = viewport;
        scrollView->_platformHelper = Helpers::GetIVRPlatformHelper();
        auto scrollTransform = scrollView->transform.cast<UnityEngine::RectTransform>();
        scrollTransform->set_anchorMin(UnityEngine::Vector2(0.0f, 0.0f));
        scrollTransform->set_anchorMax(UnityEngine::Vector2(1.0f, 1.0f));

        viewport->set_anchorMin(UnityEngine::Vector2(0, 0));
        viewport->set_anchorMax(UnityEngine::Vector2(1, 1));

        UnityEngine::GameObject* parentObject = UnityEngine::GameObject::New_ctor();
        parentObject->set_name("BSMLScrollViewContent");
        parentObject->get_transform()->SetParent(viewport, false);

        UnityEngine::UI::ContentSizeFitter* contentSize = parentObject->AddComponent<UnityEngine::UI::ContentSizeFitter*>();
        contentSize->set_horizontalFit(UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize);
        contentSize->set_verticalFit(UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize);

        UnityEngine::UI::VerticalLayoutGroup* verticalLayout = parentObject->AddComponent<UnityEngine::UI::VerticalLayoutGroup*>();
        verticalLayout->set_childForceExpandHeight(false);
        verticalLayout->set_childForceExpandWidth(false);
        verticalLayout->set_childControlHeight(true);
        verticalLayout->set_childControlWidth(true);
        verticalLayout->set_childAlignment(UnityEngine::TextAnchor::UpperCenter);

        auto rectTransform = parentObject->transform.cast<UnityEngine::RectTransform>();
        rectTransform->set_anchorMin(UnityEngine::Vector2(0, 1));
        rectTransform->set_anchorMax(UnityEngine::Vector2(1, 1));
        rectTransform->set_sizeDelta(UnityEngine::Vector2(0, 0));
        rectTransform->set_pivot(UnityEngine::Vector2(0.5f, 1));

        UnityEngine::GameObject* child = UnityEngine::GameObject::New_ctor();
        child->set_name("BSMLScrollViewContentContainer");
        child->get_transform()->SetParent(rectTransform, false);

        parentObject->AddComponent<ScrollViewContent*>()->scrollView = scrollView;

        UnityEngine::UI::VerticalLayoutGroup* layoutGroup = child->AddComponent<UnityEngine::UI::VerticalLayoutGroup*>();
        layoutGroup->set_childControlHeight(false);
        layoutGroup->set_childForceExpandHeight(false);
        layoutGroup->set_childAlignment(UnityEngine::TextAnchor::LowerCenter);
        layoutGroup->set_spacing(0.5f);

        auto externalComponents = child->AddComponent<ExternalComponents*>();
        externalComponents->components->Add(scrollView);
        externalComponents->components->Add(scrollView->get_transform());
        externalComponents->components->Add(gameObject->AddComponent<UnityEngine::UI::LayoutElement*>());

        child->transform.cast<UnityEngine::RectTransform>()->set_sizeDelta(UnityEngine::Vector2(0, -1));

        scrollView->_contentRectTransform = parentObject->transform.cast<UnityEngine::RectTransform>();
        gameObject->SetActive(true);
        return child;
    }
}
