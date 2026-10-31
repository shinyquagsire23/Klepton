#include "BSML/TypeHandlers/GraphicHandler.hpp"

namespace BSML {
    static GraphicHandler GraphicHandler{};

    GraphicHandler::Base::PropMap GraphicHandler::get_props() const {
        return {
            {"raycastPadding", {"raycast-padding"}},
            {"raycastTarget", {"raycast-target"}}
        };
    }

    GraphicHandler::Base::SetterMap GraphicHandler::get_setters() const {
        return {
            {"raycastPadding",   [](auto component, auto value){ component->raycastPadding = value; }},
            {"raycastTarget",   [](auto component, auto value){ component->raycastTarget = value; }},
        };
    }
}
