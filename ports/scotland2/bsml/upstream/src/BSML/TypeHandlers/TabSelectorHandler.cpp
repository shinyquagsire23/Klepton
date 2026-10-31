#include "BSML/TypeHandlers/TabSelectorHandler.hpp"

namespace BSML {
    static TabSelectorHandler tabSelectorHandler{};

    TabSelectorHandler::Base::PropMap TabSelectorHandler::get_props() const {
        return {
            {"tabTag", {"tab-tag"}},
            {"pageCount", {"page-count"}},
            {"leftButtonTag", {"left-button-tag"}},
            {"rightButtonTag", {"right-button-tag"}}
        };
    }

    TabSelectorHandler::Base::SetterMap TabSelectorHandler::get_setters() const {
        return {
            {"tabTag",          [](auto component, auto value){ component->tabTag = value; if (value.empty()) ERROR("Tab selector must have tab-tag!"); }},
            {"pageCount",       [](auto component, auto value){ component->set_pageCount(value); }},
            {"leftButtonTag",   [](auto component, auto value){ component->leftButtonTag = value; }},
            {"rightButtonTag",  [](auto component, auto value){ component->rightButtonTag = value; }}
        };
    }

    void TabSelectorHandler::HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        Base::HandleTypeAfterParse(componentType, parserParams);
        auto tabSelector = reinterpret_cast<TabSelector*>(componentType.component);

        tabSelector->Setup(parserParams);
    }
}