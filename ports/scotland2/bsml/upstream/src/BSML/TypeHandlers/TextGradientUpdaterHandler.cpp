#include "BSML/TypeHandlers/TextGradientUpdaterHandler.hpp"

namespace BSML {
    static TextGradientUpdaterHandler textGradientUpdaterHandler{};

    TextGradientUpdaterHandler::Base::PropMap TextGradientUpdaterHandler::get_props() const {
        return {
            { "gradientScrollSpeed",  {"gradient-speed", "gradient-scroll-speed"}},
            { "gradientScrollRepeat", {"gradient-repeat", "gradient-scroll-repeat"}},
            { "gradientColors",       {"gradient-colors", "gradient"}},
            { "gradientFixedStep",    {"gradient-fixed-step"}},
            { "gradientStepSize",     {"gradient-step-size", "gradient-fixed-step-size"}}
        };
    }

    TextGradientUpdaterHandler::Base::SetterMap TextGradientUpdaterHandler::get_setters() const {
        return {
            { "gradientScrollSpeed",    [](auto component, auto value){ component->scrollSpeed = value; }},
            { "gradientScrollRepeat",   [](auto component, auto value){ component->scrollRepeat = value; }},
            { "gradientColors",         [](auto component, auto value){ component->set_gradient(BSML::Gradient::Parse(value)); }},
            { "gradientFixedStep",      [](auto component, auto value){ component->fixedStep = value; }},
            { "gradientStepSize",       [](auto component, auto value){ component->stepSize = value; }}
        };
    }
}