#include "BSML/Tags/VerticalIconSegmentedControlTag.hpp"
#include "Helpers/getters.hpp"
#include "logging.hpp"

#include "HMUI/IconSegmentedControl.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSegmentedControlController.hpp"
#include "GlobalNamespace/PlatformLeaderboardViewController.hpp"

using namespace UnityEngine;

namespace BSML {
    static BSMLNodeParser<VerticalIconSegmentedControlTag> vericalIconSegmentedControlTag({"vertical-icon-segments"});

    HMUI::IconSegmentedControl* get_verticalIconSegmentedControlTemplate() {
        static SafePtrUnity<HMUI::IconSegmentedControl> verticalIconSegmentedControlTemplate;
        if (!verticalIconSegmentedControlTemplate) {
            auto vc = Resources::FindObjectsOfTypeAll<GlobalNamespace::PlatformLeaderboardViewController*>()->FirstOrDefault();
            verticalIconSegmentedControlTemplate = vc->_scopeSegmentedControl;
        }
        return verticalIconSegmentedControlTemplate.ptr();
    }

    UnityEngine::GameObject* VerticalIconSegmentedControlTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating VerticalIconSegmentedControl");
        auto verticalIconSegmentedControlTemplate = get_verticalIconSegmentedControlTemplate();
        auto gameObject = Helpers::GetDiContainer()->InstantiatePrefab(verticalIconSegmentedControlTemplate, parent);
        auto verticalIconSegmentedControl = gameObject->GetComponent<HMUI::IconSegmentedControl*>();
        verticalIconSegmentedControl->_dataSource = nullptr;

        gameObject->set_name("BSMLVerticalIconSegmentedControl");

        auto transform = gameObject->transform.cast<RectTransform>();
        transform->set_anchorMin({0.5f, 0.5f});
        transform->set_anchorMax({0.5f, 0.5f});
        transform->set_anchoredPosition({0, 0});
        transform->set_pivot({0.5f, 0.5f});

        int childCount = transform->get_childCount();
        for (int i = 1; i <= childCount; i++) {
            Object::DestroyImmediate(transform->GetChild(childCount - i)->get_gameObject());
        }

        gameObject->SetActive(true);
        return gameObject;
    }
}
