#include "BSML/MenuButtons/MenuButtonsViewController.hpp"

#include "BSML.hpp"
#include "assets.hpp"

DEFINE_TYPE(BSML, MenuButtonsViewController)

namespace BSML {
    bool MenuButtonsViewController::get_anyButtons() {
        return buttons ? buttons.size() > 0 : false;
    }
    
    void MenuButtonsViewController::RefreshView() {
        if (!root || !root->m_CachedPtr.m_value) {
            return;
        }

        UnityEngine::Object::DestroyImmediate(root);
        DidActivate(true, false, false);
    }

    void MenuButtonsViewController::DidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
        if (!firstActivation) return;
        parse_and_construct(Assets::MainLeftScreen_bsml, get_transform(), this);
    }
}
