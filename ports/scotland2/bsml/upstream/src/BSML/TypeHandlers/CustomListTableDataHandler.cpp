#include "BSML/TypeHandlers/CustomListTableDataHandler.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/delegates.hpp"

#include "System/Action_2.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/TextPageScrollView.hpp"
#include "HMUI/VerticalScrollIndicator.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"

using namespace UnityEngine;

HMUI::TableView::TableType stringToTableType(const std::string& str);

namespace BSML {
    static CustomListTableDataHandler customListTableDataHandler{};
    HMUI::TextPageScrollView* get_textPageTemplate();

    CustomListTableDataHandler::Base::PropMap CustomListTableDataHandler::get_props() const {
        return {
            {"selectCell", {"select-cell"}},
            {"visibleCells", {"visible-cells"}},
            {"cellSize", {"cell-size"}},
            {"id", {"id"}},
            {"data", {"data", "content"}},
            {"listWidth", {"list-width"}},
            {"listHeight", {"list-height"}},
            {"expandCell", {"expand-cell"}},
            {"listStyle", {"list-style"}},
            {"listDirection", {"list-direction"}},
            {"alignCenter", {"align-to-center"}},
            {"stickScrolling", {"stick-scrolling"}},
            {"showScrollbar", {"show-scrollbar"}}
        };
    }

    CustomListTableDataHandler::Base::SetterMap CustomListTableDataHandler::get_setters() const {
        return {
            {"expandCell",      [](auto component, auto value){ component->expandCell = value; }},
            {"alignCenter",     [](auto component, auto value){ component->tableView->_alignToCenter = value; }},
            {"stickScrolling",  [](auto component, auto value){ if (static_cast<bool>(value)) component->tableView->_scrollView->_platformHelper = Helpers::GetIVRPlatformHelper(); }}
        };
    }

    void CustomListTableDataHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        Base::HandleType(componentType, parserParams);
        auto tableData = reinterpret_cast<CustomListTableData*>(componentType.component);
        auto tableView = tableData->tableView;
        auto scrollView = tableView->scrollView;

        auto& data = componentType.data;

        auto selectCellItr = data.find("selectCell");
        if (selectCellItr != data.end() && !selectCellItr->second.empty()) {
            auto action = parserParams.TryGetAction(selectCellItr->second);
            if (action) tableView->add_didSelectCellWithIdxEvent(action->GetSystemAction<UnityW<HMUI::TableView>, int>());
            else ERROR("Action '{}' could not be found", selectCellItr->second);
        }

        bool verticalList = true;
        auto listDirectionItr = data.find("listDirection");
        if (listDirectionItr != data.end() && !listDirectionItr->second.empty()) {
            auto arg = StringParseHelper(listDirectionItr->second);
            tableView->_tableType = stringToTableType(arg);
            scrollView->_scrollViewDirection = tableView->get_tableType() == HMUI::TableView::TableType::Vertical ? HMUI::ScrollView::ScrollViewDirection::Vertical : HMUI::ScrollView::ScrollViewDirection::Horizontal;
            verticalList = tableView->get_tableType() == HMUI::TableView::TableType::Vertical;
        }

        auto listStyleItr = data.find("listStyle");
        if (listStyleItr != data.end() && !listStyleItr->second.empty()) {
            tableData->set_listStyle(CustomListTableData::stringToListStyle(listStyleItr->second));
        }

        auto cellSizeItr = data.find("cellSize");
        if (cellSizeItr != data.end() && !cellSizeItr->second.empty()) {
            tableData->cellSize = StringParseHelper(cellSizeItr->second);
        }

        auto showScrollBarItr = data.find("showScrollbar");
        // We can't show on a horizontal list
        if (verticalList && showScrollBarItr != data.end() && !showScrollBarItr->second.empty()) {
            auto arg = StringParseHelper(showScrollBarItr->second);
            if (static_cast<bool>(arg)) {
                auto textScrollView = Object::Instantiate(get_textPageTemplate(), tableData->get_transform(), false);

                auto pageUpButton = textScrollView->_pageUpButton;
                auto pageDownButton = textScrollView->_pageDownButton;

                auto scrollIndicator = textScrollView->_verticalScrollIndicator;
                auto scrollBar = scrollIndicator->transform->parent.cast<RectTransform>();

                scrollView->_pageUpButton = pageUpButton;
                scrollView->_pageDownButton = pageDownButton;
                scrollView->_verticalScrollIndicator = scrollIndicator;
                scrollBar->SetParent(tableData->get_transform());

                Object::Destroy(textScrollView->get_gameObject());

                scrollBar->set_anchorMin({1, 0});
                scrollBar->set_anchorMax({1, 1});
                scrollBar->set_offsetMin({0, 0});
                scrollBar->set_offsetMax({8, 0});
            }
        }

        auto dataItr = data.find("data");
        if (dataItr != data.end() && !dataItr->second.empty()) {
            auto val = parserParams.TryGetValue(dataItr->second);
            List<BSML::CustomCellInfo*>* cellInfos = val ? val->GetValue<List<BSML::CustomCellInfo*>*>() : nullptr;
            static auto dataKlass = classof(List<BSML::CustomCellInfo*>*);
            if (cellInfos && il2cpp_functions::class_is_assignable_from(cellInfos->klass, dataKlass)) {
                tableData->data = cellInfos;
                tableView->ReloadData();
            } else if (cellInfos && !il2cpp_functions::class_is_assignable_from(cellInfos->klass, dataKlass)){
                ERROR("The class of the data field was not Correct! this should be a 'List<BSML::CustomCellInfo*>*' or equivalent!");
            } else {
                ERROR("IconSegmentedControl needs to have at least 1 value!");
                ERROR("This means BSML could not find value '{}'", dataItr->second);
            }
        }

        INFO("set sizeDelta");
        auto transform = tableData->transform.cast<RectTransform>();
        switch(tableView->get_tableType()) {
            case HMUI::TableView::TableType::Vertical: {
                auto listWidthItr = data.find("listWidth");
                auto listWidth = listWidthItr != data.end() ? static_cast<float>(StringParseHelper(listWidthItr->second)) : 60.0f;
                auto visibleCellsItr = data.find("visibleCells");
                auto visibleCells = visibleCellsItr != data.end() ? static_cast<int>(StringParseHelper(visibleCellsItr->second)) : 7;
                float listHeight = visibleCells * tableData->cellSize;
                transform->set_sizeDelta({listWidth, listHeight});
                INFO("listWidth: {}, listHeight: {}", listWidth, listHeight);
                break;
            }
            case HMUI::TableView::TableType::Horizontal: {
                auto listHeightItr = data.find("listHeight");
                auto listHeight = listHeightItr != data.end() ? static_cast<float>(StringParseHelper(listHeightItr->second)) : 40.0f;
                auto visibleCellsItr = data.find("visibleCells");
                auto visibleCells = visibleCellsItr != data.end() ? static_cast<int>(StringParseHelper(visibleCellsItr->second)) : 4;
                float listWidth = visibleCells * tableData->cellSize;
                INFO("listWidth: {}, listHeight: {}", listWidth, listHeight);
                transform->set_sizeDelta({listWidth, listHeight});
                break;
            }
        }

        auto layoutElement = tableData->GetComponent<UnityEngine::UI::LayoutElement*>();
        if (layoutElement) {
            auto sizeDelta = transform->get_sizeDelta();
            INFO("Size: {}, {}", sizeDelta.x, sizeDelta.y);
            layoutElement->set_preferredHeight(sizeDelta.y);
            layoutElement->set_flexibleHeight(sizeDelta.y);
            layoutElement->set_minHeight(sizeDelta.y);
            layoutElement->set_preferredWidth(sizeDelta.x);
            layoutElement->set_flexibleWidth(sizeDelta.x);
            layoutElement->set_minWidth(sizeDelta.x);
        }

        tableView->get_gameObject()->SetActive(true);
        tableView->LazyInit();

        auto idItr = data.find("id");
        if (idItr != data.end() && !idItr->second.empty()) {
            std::string id = idItr->second;
            auto pageUpMinfo = il2cpp_utils::FindMethodUnsafe(scrollView, "PageUpButtonPressed", 0);
            auto pageDownMinfo = il2cpp_utils::FindMethodUnsafe(scrollView, "PageDownButtonPressed", 0);

            if (pageUpMinfo) parserParams.AddAction(id + "#PageUp", new BSMLAction(scrollView, pageUpMinfo));
            if (pageDownMinfo) parserParams.AddAction(id + "#PageDown", new BSMLAction(scrollView, pageDownMinfo));
        }
    }
}

HMUI::TableView::TableType stringToTableType(const std::string& str) {
    if (str == "Horizontal") return HMUI::TableView::TableType::Horizontal;
    return HMUI::TableView::TableType::Vertical;
}
