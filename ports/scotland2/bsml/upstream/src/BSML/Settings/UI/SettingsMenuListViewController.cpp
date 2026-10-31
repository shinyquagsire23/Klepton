#include "BSML/Settings/UI/SettingsMenuListViewController.hpp"
#include "BSML/Settings/BSMLSettings.hpp"

#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/RectTransform.hpp"

#include "logging.hpp"
#include "assets.hpp"
#include "BSML.hpp"

DEFINE_TYPE(BSML, SettingsMenuListViewController);

namespace BSML {
    void SettingsMenuListViewController::DidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
        if (firstActivation) {
            DEBUG("DidActivate");
            parse_and_construct(Assets::Settings::List_bsml, get_transform(), this);

            auto r = get_rectTransform();
            r->set_sizeDelta({35, 0});
            r->set_anchorMin({0.5f, 0});
            r->set_anchorMax({0.5f, 1});
        }

        DEBUG("Update Data: {}", fmt::ptr(list));
        if (list && list->m_CachedPtr.m_value) {
            list->data = BSMLSettings::get_instance()->get_settingsMenus();
            if (list->tableView && list->tableView->m_CachedPtr.m_value) {
                list->tableView->ReloadData();
            }
        }
    }

    void SettingsMenuListViewController::SettingsClick(HMUI::TableView* tableView, int idx) {
        INFO("Settings Click");
        auto menu = reinterpret_cast<SettingsMenu*>(list->data[idx]);
        if (clickedMenu) clickedMenu(menu);
    }
}
