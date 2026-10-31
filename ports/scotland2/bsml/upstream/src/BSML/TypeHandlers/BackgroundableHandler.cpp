#include "BSML/TypeHandlers/BackgroundableHandler.hpp"
#include "Helpers/utilities.hpp"

namespace BSML {
    static BackgroundableHandler backgroundableHandler{};

    BackgroundableHandler::Base::PropMap BackgroundableHandler::get_props() const {
        return {
            {"background", {"bg", "background"}},
            {"backgroundColor", {"bg-color", "background-color"}},
            {"backgroundColor0", {"bg-color0", "background-color0"}},
            {"backgroundColor1", {"bg-color1", "background-color1"}},
            {"backgroundAlpha", {"bg-alpha", "background-alpha"}}
        };
    }

    BackgroundableHandler::Base::SetterMap BackgroundableHandler::get_setters() const {
        return {
            {"background", [](auto component, auto value){ component->ApplyBackground(value); }},
            {"backgroundColor", [](auto component, auto value){ component->ApplyColor(value); }},
            {"backgroundColor0", [](auto component, auto value){ component->ApplyColor0(value); }},
            {"backgroundColor1", [](auto component, auto value){ component->ApplyColor1(value); }},
            {"backgroundAlpha", [](auto component, auto value){ component->ApplyAlpha(value); }}
        };
    }
}