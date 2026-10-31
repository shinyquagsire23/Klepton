#include "BSML/Tags/SettingsContainerTag.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "logging.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"

namespace BSML {
    static BSMLNodeParser<SettingsContainerTag> settingsContainerTagParser({"settings-scroll-view", "scrollable-settings-container", "settings-container"});

    UnityEngine::GameObject* SettingsContainerTag::CreateObject(UnityEngine::Transform* parent) const {
        INFO("Creating settings container");
        UnityEngine::GameObject* content = this->ScrollViewTag::CreateObject(parent);
        auto components = content->GetComponent<ExternalComponents*>();
        UnityEngine::RectTransform* scrollTransform = components->Get<UnityEngine::RectTransform*>();
        scrollTransform->set_anchoredPosition(UnityEngine::Vector2(0, 0));
        scrollTransform->set_sizeDelta(UnityEngine::Vector2(0, -20));
        scrollTransform->get_gameObject()->set_name("BSMLScrollableSettingsContainer");
        return content;
    }
}