#include "BSML/GameplaySetup/GameplaySetupCell.hpp"
#include "logging.hpp"

#include "BSML/Components/ExternalComponents.hpp"

DEFINE_TYPE(BSML, GameplaySetupCell);

namespace BSML {
    GameplaySetupCell* GameplaySetupCell::PopulateCell(GameplaySetupMenu* menu) {
        DEBUG("Populating GameplaySetupCell");
        tab = menu;

        toggle->set_text(get_name());
        toggle->ReceiveValue();

        return this;
    }

    StringW GameplaySetupCell::get_name() {
        return tab ? tab->name : "";
    }
    bool GameplaySetupCell::get_visible() {
        return tab && tab->get_visible();
    }

    void GameplaySetupCell::set_visible(bool value) {
        if (tab) {
            tab->set_visible(value);
            toggle->ReceiveValue();
        }
    }
}