#pragma once

#include <string_view>

#include "HMUI/ImageView.hpp"
#include "HMUI/TableCell.hpp"
#include "HMUI/TableView.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "custom-types/shared/macros.hpp"

#include "Airbuds/Friend.hpp"

DECLARE_CLASS_CODEGEN(AirbudsSearch::UI, FriendListTableViewCell, HMUI::TableCell) {

    DECLARE_CTOR(ctor);

    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);

    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, root_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, friendNameTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, friendHandleTextView_);

    DECLARE_INSTANCE_METHOD(void, OnDestroy);

    public:
    static constexpr std::string_view CELL_REUSE_ID = "FriendListTableViewCell";
    void setFriend(const airbuds::Friend& friendUser);

    private:
    void updateBackground();

    airbuds::Friend friend_;
};
