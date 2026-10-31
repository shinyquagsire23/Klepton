#include "BSML/Components/Tab.hpp"
#include "BSML/Components/TabSelector.hpp"

DEFINE_TYPE(BSML, Tab);

namespace BSML {
    void Tab::ctor() {
        isVisible = true;
    }

    bool Tab::get_isVisible() {
        return isVisible;
    }

    void Tab::set_isVisible(bool value) {
        isVisible = value;
        if (selector) selector->Refresh();
    }

    StringW Tab::get_tabName() {
        return tabName ? tabName : "";
    }

    void Tab::set_tabName(StringW value) {
        tabName = value;
        if (selector) selector->Refresh();
    }

    StringW Tab::get_tabKey() {
        return tabKey ? tabKey : "";
    }

    void Tab::set_tabKey(StringW value) {
        tabKey = value;
        if (selector) selector->Refresh();
    }

    TabSelector* Tab::get_selector() { return reinterpret_cast<TabSelector*>(_selector); }
    TabSelector const* Tab::get_selector() const { return reinterpret_cast<TabSelector const*>(_selector); }
    void Tab::set_selector(TabSelector* value) { _selector = value; }
}
