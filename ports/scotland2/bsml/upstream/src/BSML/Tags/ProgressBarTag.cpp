#include "BSML/Tags/ProgressBarTag.hpp"
#include "BSML/Components/ProgressBar.hpp"
#include "BSML/Components/ExternalComponents.hpp"

#include "UnityEngine/Vector3.hpp"

namespace BSML {
    BSMLNodeParser<ProgressBarTag> progressBarTagParser({"progress-bar"});

    UnityEngine::GameObject* ProgressBarTag::CreateObject(UnityEngine::Transform *parent) const {
        auto bar = ProgressBar::CreateProgressBar({}, {1, 1, 1}, {}, "");
        auto gameObject = bar->get_gameObject();
        bar->get_transform()->SetParent(parent, false);

        auto externalComponents = gameObject->AddComponent<BSML::ExternalComponents*>();

        externalComponents->Add(bar->headerText);
        externalComponents->Add(bar->loadingBar);

        return gameObject;
    }
}
