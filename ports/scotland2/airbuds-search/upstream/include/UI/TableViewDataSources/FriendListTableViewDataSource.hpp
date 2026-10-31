#pragma once

#include <string_view>
#include <vector>

#include "HMUI/TableCell.hpp"
#include "HMUI/TableView.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "custom-types/shared/macros.hpp"

#include "Airbuds/Friend.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI, FriendListTableViewDataSource, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {

    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView* tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

    public:
    std::vector<airbuds::Friend> friends_;
    int getIndexForFriendId(std::string_view friendId) const;
};
