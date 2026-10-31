#include "BSML/Tags/LoadingIndicatorTag.hpp"
#include "logging.hpp"

#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "HMUI/ImageView.hpp"
#include "GlobalNamespace/LevelCollectionNavigationController.hpp"
#include "GlobalNamespace/LoadingControl.hpp"

#include "Helpers/getters.hpp"

using namespace UnityEngine;

namespace BSML {
    static BSMLNodeParser<LoadingIndicatorTag> loadingIndicatorTagParser({"loading", "loading-indicator"});

    UnityEngine::GameObject* get_loadingTemplate() {
        static SafePtrUnity<UnityEngine::GameObject> loadingTemplate;
        if (!loadingTemplate) {
            loadingTemplate = Helpers::GetDiContainer()->Resolve<GlobalNamespace::LevelCollectionNavigationController*>()->_loadingControl->_loadingContainer->transform->Find("LoadingIndicator")->get_gameObject();
        }
        if (!loadingTemplate) {
            ERROR("No loading template found!");
            return nullptr;
        }
        return loadingTemplate.ptr();
    }
    UnityEngine::GameObject* LoadingIndicatorTag::CreateObject(UnityEngine::Transform* parent) const {

        auto gameObject = Object::Instantiate(get_loadingTemplate(), parent, false);
        gameObject->set_name("BSMLLoadingIndicator");

        gameObject->AddComponent<UI::LayoutElement*>();
        return gameObject;
    }
}
