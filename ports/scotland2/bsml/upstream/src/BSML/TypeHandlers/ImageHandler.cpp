#include "BSML/TypeHandlers/ImageHandler.hpp"
#include "Helpers/utilities.hpp"

namespace BSML {
    static ImageHandler imageHandler{};

    ImageHandler::Base::PropMap ImageHandler::get_props() const {
        return {
            {"image", {"source", "src"}},
            {"preserveAspect", {"preserve-aspect"}},
            {"imageColor", {"image-color", "img-color"}}
        };
    }
    ImageHandler::Base::SetterMap ImageHandler::get_setters() const {
        return {
            {"image",           [](auto component, auto value){ Utilities::SetImage(component, value); }},
            {"preserveAspect",  [](auto component, auto value){ component->set_preserveAspect(value); }},
            {"imageColor",      [](auto component, auto value){ component->set_color(value); }}
        };
    }
}