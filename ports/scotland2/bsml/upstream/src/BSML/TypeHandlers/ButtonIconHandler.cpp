#include "BSML/TypeHandlers/ButtonIconHandler.hpp"

namespace BSML {
    static ButtonIconHandler buttonIconHandler;

    ButtonIconHandler::Base::PropMap ButtonIconHandler::get_props() const {
        return {
            {"icon", {"icon"}}
        };
    }

    ButtonIconHandler::Base::SetterMap ButtonIconHandler::get_setters() const {
        return {
            {"icon", [](auto component, auto value){ component->SetIcon(value); }}
        };
    }
}