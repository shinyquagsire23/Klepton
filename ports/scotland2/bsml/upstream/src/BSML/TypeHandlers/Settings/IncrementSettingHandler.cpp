#include "BSML/TypeHandlers/Settings/IncrementSettingHandler.hpp"

namespace BSML {
    static IncrementSettingHandler incrementSettingHandler{};

    IncrementSettingHandler::Base::PropMap IncrementSettingHandler::get_props() const {
        return {
            {"increment", {"increment"}},
            {"minValue", {"min"}},
            {"maxValue", {"max"}},
            {"isInt", {"integer-only"}},
            {"digits", {"digits"}}
        };
    }

    IncrementSettingHandler::Base::SetterMap IncrementSettingHandler::get_setters() const {
        return {
            {"increment",   [](auto component, auto value){ auto v = value.tryParseFloat(); if (v.has_value()) component->increments = v.value(); }},
            {"minValue",   [](auto component, auto value){ auto v = value.tryParseFloat(); if (v.has_value()) component->minValue = v.value(); }},
            {"maxValue",   [](auto component, auto value){ auto v = value.tryParseFloat(); if (v.has_value()) component->maxValue = v.value(); }},
            {"isInt",   [](auto component, auto value){ auto v = value.tryParseBool(); if (v.has_value()) component->isInt = v.value(); }},
            {"digits",   [](auto component, auto value){ auto v = value.tryParseInt(); if (v.has_value()) component->digits = v.value(); }},
        };
    }
}