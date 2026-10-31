#include "BSML/TypeHandlers/ContentSizeFitterHandler.hpp"

static std::map<std::string, UnityEngine::UI::ContentSizeFitter::FitMode> stringToFitModeMap {
    {"Unconstrained", UnityEngine::UI::ContentSizeFitter::FitMode::Unconstrained},
    {"MinSize", UnityEngine::UI::ContentSizeFitter::FitMode::MinSize},
    {"PreferredSize", UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize}
};

static std::optional<UnityEngine::UI::ContentSizeFitter::FitMode> stringToFitMode(const std::string& str) {
    auto itr = stringToFitModeMap.find(str);
    if (itr != stringToFitModeMap.end()) {
        return itr->second;
    }

    return std::nullopt;
}

namespace BSML {
    static ContentSizeFitterHandler contentSizeFitterHandler{};

    ContentSizeFitterHandler::Base::PropMap ContentSizeFitterHandler::get_props() const {
        return {
            {"horizontalFit", {"horizontal-fit"}},
            {"verticalFit", {"vertical-fit"}}
        };
    }

    ContentSizeFitterHandler::Base::SetterMap ContentSizeFitterHandler::get_setters() const {
        return {
            {"horizontalFit", [](auto component, auto value){ auto v = stringToFitMode(value); if (v.has_value()) component->set_horizontalFit(v.value()); }},
            {"verticalFit", [](auto component, auto value){ auto v = stringToFitMode(value); if (v.has_value()) component->set_verticalFit(v.value()); }}
        };
    }
}