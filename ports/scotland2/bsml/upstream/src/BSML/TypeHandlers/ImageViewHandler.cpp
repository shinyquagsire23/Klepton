#include "BSML/TypeHandlers/ImageViewHandler.hpp"

namespace BSML {
    static ImageViewHandler imageViewHandler{};

    ImageViewHandler::Base::PropMap ImageViewHandler::get_props() const {
        return {
            {"skew", {"skew"}}
        };
    }
    ImageViewHandler::Base::SetterMap ImageViewHandler::get_setters() const {
        return {
            {"skew", [](auto component, auto value){ component->_skew = value; }}
        };
    }
}
