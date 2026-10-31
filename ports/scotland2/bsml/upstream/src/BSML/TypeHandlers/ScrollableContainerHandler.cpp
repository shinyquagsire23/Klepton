#include "BSML/TypeHandlers/ScrollableContainerHandler.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "HMUI/VerticalScrollIndicator.hpp"
#include "HMUI/ItemForFocussedScrolling.hpp"

namespace BSML {
    static ScrollableContainerHandler scrollableContainerHandler{};

    ScrollableContainerHandler::Base::PropMap ScrollableContainerHandler::get_props() const {
        return {
            {"id", {"id"}},
            {"maskOverflow", {"mask-overflow"}},
            {"alignBottom", { "align-bottom"}},
            {"scrollToBottomOnUpdate", { "scroll-to-bottom-on-update"}},
        };
    }

    ScrollableContainerHandler::Base::SetterMap ScrollableContainerHandler::get_setters() const {
        return {
            {"maskOverflow", [](auto component, auto value){ component->set_maskOverflow(value); }},
            {"alignBottom", [](auto component, auto value){ component->set_alignBottom(value); }},
            {"scrollToBottomOnUpdate", [](auto component, auto value){ component->scrollToBottomOnUpdate = value; }}
        };
    }

    void ScrollableContainerHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        Base::HandleType(componentType, parserParams);
        auto scrollView = reinterpret_cast<ScrollableContainer*>(componentType.component);
        auto& data = componentType.data;
        auto idItr = data.find("id");
        if (idItr != data.end() && !idItr->second.empty()) {
            std::string id = idItr->second;
            auto pageUpMinfo = il2cpp_utils::FindMethodUnsafe(scrollView, "PageUpButtonPressed", 0);
            auto pageDownMinfo = il2cpp_utils::FindMethodUnsafe(scrollView, "PageDownButtonPressed", 0);

            if (pageUpMinfo) parserParams.AddAction(id + "#PageUp", new BSMLAction(scrollView, pageUpMinfo));
            if (pageDownMinfo) parserParams.AddAction(id + "#PageDown", new BSMLAction(scrollView, pageDownMinfo));
        }
    }

    void ScrollableContainerHandler::HandleTypeAfterChildren(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        auto& gos = parserParams.GetObjectsWithTag("ScrollFocus");
        for (auto go : gos) {
            go->AddComponent<HMUI::ItemForFocussedScrolling*>();
        }
    }

    void ScrollableContainerHandler::HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        INFO("After parse");
        auto scrollView = reinterpret_cast<ScrollableContainer*>(componentType.component);
        auto& data = componentType.data;

        auto idItr = data.find("id");
        if (idItr != data.end() && !idItr->second.empty()) {
            auto pageUpButtons = parserParams.GetObjectsWithTag("PageUpFor:" + idItr->second);
            for (auto pageUp : pageUpButtons) {
                scrollView->_pageUpButton = pageUp->GetComponent<UnityEngine::UI::Button*>();
                if (scrollView->_pageUpButton) break;
            }

            auto pageDownButtons = parserParams.GetObjectsWithTag("PageDownFor:" + idItr->second);
            for (auto pageDown : pageDownButtons) {
                scrollView->_pageDownButton = pageDown->GetComponent<UnityEngine::UI::Button*>();
                if (scrollView->_pageDownButton) break;
            }

            auto scrollIndicators = parserParams.GetObjectsWithTag("IndicatorFor:" + idItr->second);
            for (auto scrollIndicator : scrollIndicators) {
                scrollView->_verticalScrollIndicator = scrollIndicator->GetComponent<HMUI::VerticalScrollIndicator*>();
                if (scrollView->_verticalScrollIndicator) break;
            }
            INFO("upButton: {}", fmt::ptr(scrollView->_pageUpButton.unsafePtr()));
            INFO("downButton: {}", fmt::ptr(scrollView->_pageDownButton.unsafePtr()));
            INFO("scrollIndicator: {}", fmt::ptr(scrollView->_verticalScrollIndicator.unsafePtr()));
        }

        scrollView->RefreshContent();
        scrollView->RefreshButtons();
    }
}
