#include "BSML/TypeHandlers/LayoutGroupHandler.hpp"
#include "UnityEngine/RectOffset.hpp"

static std::map<std::string, UnityEngine::TextAnchor> stringToTextAnchorMap {
    {"UpperLeft", UnityEngine::TextAnchor::UpperLeft},
    {"UpperCenter", UnityEngine::TextAnchor::UpperCenter},
    {"UpperRight", UnityEngine::TextAnchor::UpperRight},
    {"MiddleLeft", UnityEngine::TextAnchor::MiddleLeft},
    {"MiddleCenter", UnityEngine::TextAnchor::MiddleCenter},
    {"MiddleRight", UnityEngine::TextAnchor::MiddleRight},
    {"LowerLeft", UnityEngine::TextAnchor::LowerLeft},
    {"LowerCenter", UnityEngine::TextAnchor::LowerCenter},
    {"LowerRight", UnityEngine::TextAnchor::LowerRight}
};

static std::optional<UnityEngine::TextAnchor> stringToTextAnchor(const std::string& str) {
    auto itr = stringToTextAnchorMap.find(str);
    if (itr != stringToTextAnchorMap.end()) {
        return itr->second;
    }

    return std::nullopt;
}

namespace BSML {
    static LayoutGroupHandler layoutGroupHandler{};

    LayoutGroupHandler::Base::PropMap LayoutGroupHandler::get_props() const {
        return {
            {"padTop", {"pad-top"}},
            {"padBottom", {"pad-bottom"}},
            {"padLeft", {"pad-left"}},
            {"padRight", {"pad-right"}},
            {"pad", {"pad"}},
            {"childAlign",  {"child-align"}}
        };
    }

    LayoutGroupHandler::Base::SetterMap LayoutGroupHandler::get_setters() const {
        return {

        };
    }

    void LayoutGroupHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        auto layoutGroup = reinterpret_cast<UnityEngine::UI::LayoutGroup*>(componentType.component);
        auto& data = componentType.data;

        auto padItr = data.find("pad");
        if (padItr != data.end()) {
            int pad = StringParseHelper(padItr->second);
            layoutGroup->set_padding(UnityEngine::RectOffset::New_ctor(pad, pad, pad, pad));
        }

        auto padTopItr = data.find("padTop");
        auto padBottomItr = data.find("padBottom");
        auto padLeftItr = data.find("padLeft");
        auto padRightItr = data.find("padRight");
        auto originalPadding = layoutGroup->get_padding();

        // if any padding values are found, override them
        layoutGroup->set_padding(UnityEngine::RectOffset::New_ctor(
            padLeftItr == data.end()     ? originalPadding->get_left()   : StringParseHelper(padLeftItr->second), 
            padRightItr == data.end()    ? originalPadding->get_right()  : StringParseHelper(padRightItr->second),
            padTopItr == data.end()      ? originalPadding->get_top()    : StringParseHelper(padTopItr->second), 
            padBottomItr == data.end()   ? originalPadding->get_bottom() : StringParseHelper(padBottomItr->second) 
        ));

        auto childAlignItr = data.find("childAlign");
        if (childAlignItr != data.end()) {
            auto childAlign = stringToTextAnchor(childAlignItr->second);
            if (childAlign.has_value()) layoutGroup->set_childAlignment(childAlign.value());
        }

        Base::HandleType(componentType, parserParams);
    }
}
