#include "BSML/TypeHandlers/TabHandler.hpp"

namespace BSML {
    static TabHandler tabHandler{};

    TabHandler::Base::PropMap TabHandler::get_props() const {
        return {
            { "tabName", {"tab-name"}},
            { "tabNameKey", {"tab-name-key"}}
        };
    }

    TabHandler::Base::SetterMap TabHandler::get_setters() const {
        return {
            { "tabName",    [](auto component, auto value){ component->set_tabName(value); if (value.empty()) ERROR("Tabname can not be empty!"); }},
            { "tabNameKey", [](auto component, auto value){ component->set_tabKey(value); }}
        };
    }
}
