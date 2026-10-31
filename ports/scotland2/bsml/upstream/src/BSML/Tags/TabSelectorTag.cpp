#include "BSML/Tags/TabSelectorTag.hpp"
#include "logging.hpp"

#include "BSML/Components/TabSelector.hpp"
#include "HMUI/TextSegmentedControl.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "Zenject/DiContainer.hpp"
#include "GlobalNamespace/PlayerStatisticsViewController.hpp"

#include "Helpers/getters.hpp"

using namespace UnityEngine;

namespace BSML {
    static BSMLNodeParser<TabSelectorTag> tabSelectorTagParser({"tab-select", "tab-selector"});

    HMUI::TextSegmentedControl* get_tabSelectorTagTemplate() {
        static SafePtrUnity<HMUI::TextSegmentedControl> tabSelectorTagTemplate;
        if (!tabSelectorTagTemplate) {
            tabSelectorTagTemplate = Helpers::GetDiContainer()->Resolve<GlobalNamespace::PlayerStatisticsViewController*>()->_statsScopeSegmentedControl;
        }
        if (!tabSelectorTagTemplate) {
            ERROR("No tabSelectorTagTemplate found!");
            return nullptr;
        }
        return tabSelectorTagTemplate.ptr();
    }

    UnityEngine::GameObject* TabSelectorTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating TabSelector");

        auto tabTemplate = get_tabSelectorTagTemplate();
        auto diContainer = tabTemplate->_container;
        auto textSegmentedControl = diContainer->InstantiatePrefabForComponent<HMUI::TextSegmentedControl*>(tabTemplate, parent);
        auto gameObject = textSegmentedControl->get_gameObject();

        gameObject->set_name("BSMLTabSelector");

        auto transform = gameObject->transform.cast<RectTransform>();
        transform->set_anchoredPosition({0, 0});
        int childCount = transform->get_childCount();
        for (int i = 1; i <= childCount; i++) {
            Object::DestroyImmediate(transform->GetChild(childCount - i)->get_gameObject());
        }

        gameObject->AddComponent<TabSelector*>()->textSegmentedControl = textSegmentedControl;
        gameObject->SetActive(true);

        return gameObject;
    }
}
