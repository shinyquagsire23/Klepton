#include "BSML/TypeHandlers/SelectableHandler.hpp"

namespace BSML {
    static SelectableHandler selectableHandler{};

    SelectableHandler::Base::PropMap SelectableHandler::get_props() const {
        return {
            {"interactable", {"interactable"}}
        };
    }

    SelectableHandler::Base::SetterMap SelectableHandler::get_setters() const {
        return {
            {"interactable", [](auto component, auto value){ component->set_interactable(value); }}
        };
    }
}