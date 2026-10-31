#pragma once

#include "HMUI/ImageView.hpp"
#include "HMUI/TableCell.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include <string>
#include "bsml/shared/macros.hpp"
#include "custom-types/shared/macros.hpp"

DECLARE_CLASS_CODEGEN(AirbudsSearch::UI, AirbudsTrackHeaderTableViewCell, HMUI::TableCell) {

    DECLARE_CTOR(ctor);

    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);

    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, root_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, headerTextView_);

    public:
    static constexpr std::string_view CELL_REUSE_ID = "AirbudsTrackHeaderTableViewCell";
    void setHeader(const std::string& text);
};
