#include "BSML/Tags/TabTag.hpp"
#include "logging.hpp"

#include "BSML/Components/Tab.hpp"

using namespace UnityEngine;

namespace BSML {
    static BSMLNodeParser<TabTag> tabSelectorTagParser({"tab"});

    UnityEngine::GameObject* TabTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Tab");
        
        auto gameObject = Base::CreateObject(parent);
        gameObject->set_name("BSMLTab");
        gameObject->AddComponent<Tab*>();
        return gameObject;
    }
}