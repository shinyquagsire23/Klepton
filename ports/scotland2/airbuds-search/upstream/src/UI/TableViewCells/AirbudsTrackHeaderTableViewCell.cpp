#include "UnityEngine/Color.hpp"

#include "UI/TableViewCells/AirbudsTrackHeaderTableViewCell.hpp"

DEFINE_TYPE(AirbudsSearch::UI, AirbudsTrackHeaderTableViewCell);

using namespace AirbudsSearch::UI;

void AirbudsTrackHeaderTableViewCell::ctor() {
    INVOKE_BASE_CTOR(classof(HMUI::TableCell*));
}

void AirbudsTrackHeaderTableViewCell::WasPreparedForReuse() {
}

void AirbudsTrackHeaderTableViewCell::setHeader(const std::string& text) {
    if (headerTextView_) {
        headerTextView_->set_text(text);
    }
    if (root_) {
        root_->set_color(UnityEngine::Color(0.0f, 0.0f, 0.0f, 0.55f));
    }
}
