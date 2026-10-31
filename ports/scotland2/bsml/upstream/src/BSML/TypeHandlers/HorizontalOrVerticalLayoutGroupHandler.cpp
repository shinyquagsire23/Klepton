#include "BSML/TypeHandlers/HorizontalOrVerticalLayoutGroupHandler.hpp"

namespace BSML {
    static HorizontalOrVerticalLayoutGroupHandler horizontalOrVerticalLayoutGroupHandler{};
    
    HorizontalOrVerticalLayoutGroupHandler::Base::PropMap HorizontalOrVerticalLayoutGroupHandler::get_props() const {
        return {
            {"spacing", {"spacing"}},
            {"childForceExpandWidth", {"child-expand-width"}},
            {"childForceExpandHeight", {"child-expand-height"}},
            {"childControlWidth", {"child-control-width"}},
            {"childControlHeight", {"child-control-height"}}
        };
    }

    HorizontalOrVerticalLayoutGroupHandler::Base::SetterMap HorizontalOrVerticalLayoutGroupHandler::get_setters() const {
        return {
            {"spacing",                 [](auto component, auto value){ component->set_spacing(value); }},
            {"childForceExpandWidth",   [](auto component, auto value){ component->set_childForceExpandWidth(value); }},
            {"childForceExpandHeight",  [](auto component, auto value){ component->set_childForceExpandHeight(value); }},
            {"childControlWidth",       [](auto component, auto value){ component->set_childControlWidth(value); }},
            {"childControlHeight",      [](auto component, auto value){ component->set_childControlHeight(value); }}
        };
    }
}