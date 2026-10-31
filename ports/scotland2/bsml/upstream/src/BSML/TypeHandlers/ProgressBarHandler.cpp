#include "BSML/TypeHandlers/ProgressBarHandler.hpp"

namespace BSML {
    static ProgressBarHandler rawImageHandler{};

    ProgressBarHandler::Base::PropMap ProgressBarHandler::get_props() const {
        return {
            { "subText1", {"sub-text-1", "sub-1"}},
            { "subText2", {"sub-text-2", "sub-2"}}
        };
    }

    ProgressBarHandler::Base::SetterMap ProgressBarHandler::get_setters() const {
        return {
            { "subText1", [](auto component, auto value){ component->subText1->set_text(value); }},
            { "subText2", [](auto component, auto value){ component->subText2->set_text(value); }}
        };
    }
}
