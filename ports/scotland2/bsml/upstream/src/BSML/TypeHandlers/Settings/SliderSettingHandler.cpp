#include "BSML/TypeHandlers/Settings/SliderSettingHandler.hpp"

namespace BSML {
    static SliderSettingHandler sliderSettingHandler{};

    SliderSettingHandler::Base::PropMap SliderSettingHandler::get_props() const {
        return {
            {"increment", {"increment"}},
            {"minValue", {"min"}},
            {"maxValue", {"max"}},
            {"isInt", {"integer-only"}},
            {"digits", {"digits"}},
            {"showButtons", {"show-buttons"}}
        };
    }

    SliderSettingHandler::Base::SetterMap SliderSettingHandler::get_setters() const {
        return {
            {"increment", [](auto component, auto value){ component->increments = value; }},
            {"minValue", [](auto component, auto value){ component->slider->set_minValue(value); }},
            {"maxValue", [](auto component, auto value){ component->slider->set_maxValue(value); }},
            {"isInt", [](auto component, auto value){ component->isInt = value; }},
            {"digits", [](auto component, auto value){ component->digits = value; }},
            {"showButtons", [](auto component, auto value){ component->showButtons = value; }}
        };
    }
}