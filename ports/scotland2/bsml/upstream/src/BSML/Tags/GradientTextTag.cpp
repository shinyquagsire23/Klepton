#include "BSML/Tags/GradientTextTag.hpp"
#include "BSML/Components/TextGradientUpdater.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<GradientTextTag> textTagParser({"gradient-text"});

    UnityEngine::GameObject* GradientTextTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Gradient Text");
        auto gameObject = TextTag::CreateObject(parent);
        gameObject->AddComponent<BSML::TextGradientUpdater*>();
        return gameObject;
    }
}