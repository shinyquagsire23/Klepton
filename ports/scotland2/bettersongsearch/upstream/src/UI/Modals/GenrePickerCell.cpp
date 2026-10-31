#include "UI/Modals/GenrePickerCell.hpp"

DEFINE_TYPE(BetterSongSearch::UI::Modals, GenrePickerCell);

std::string const GenrePickerCellReuseIdentifier = "REUSEGenreTableCell";

namespace BetterSongSearch::UI::Modals {
    void GenrePickerCell::Refresh() {
        if (genre == nullptr) {
            return;
        }

        includeButton->set_fontStyle(genre->status == GenreCellStatus::Exclude ? TMPro::FontStyles::Strikethrough : TMPro::FontStyles::Normal);

        includeButton->set_color(UnityEngine::Color(
            genre->status == GenreCellStatus::Exclude ? 1.0f : 0.8f,
            genre->status == GenreCellStatus::Include ? 1.0f : 0.7f,
            0.8f,
            get_highlighted() ? 1.0f : 0.8f
        ));

        excludeButton->get_gameObject()->SetActive(highlighted && genre->status != GenreCellStatus::Exclude);
    }

    void GenrePickerCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        Refresh();
    }

    void GenrePickerCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        Refresh();
    }

    void GenrePickerCell::WasPreparedForReuse() {
        includeButton->set_text("");
        genre = nullptr;
    }

    BetterSongSearch::UI::Modals::GenrePickerCell* GenrePickerCell::PopulateWithGenre(BetterSongSearch::UI::Modals::GenreCellState* state) {
        this->genre = state;
        includeButton->set_text(fmt::format("{} ({})", state->tag, state->songCount));
        Refresh();

        return this;
    }

    BetterSongSearch::UI::Modals::GenrePickerCell* GenrePickerCell::GetCell(HMUI::TableView* tableView) {
        auto tableCell = tableView->DequeueReusableCellForIdentifier(GenrePickerCellReuseIdentifier);
        if (!tableCell) {
            tableCell = UnityEngine::GameObject::New_ctor("GenrePickerCell")->AddComponent<GenrePickerCell*>();
            tableCell->set_interactable(true);
            tableCell->set_reuseIdentifier(GenrePickerCellReuseIdentifier);
            BSML::parse_and_construct(Assets::GenrePickerCell_bsml, tableCell->get_transform(), tableCell);

            // Weird hack cause HMUI touchable is not there for some reason, thanks RedBrumbler
            tableCell->get_gameObject()->AddComponent<HMUI::Touchable*>();
        }

        return tableCell.cast<GenrePickerCell>();
    }

    void GenrePickerCell::IncludeGenre() {
        if (genre == nullptr) {
            return;
        }
        genre->status = genre->status == GenreCellStatus::Include ? GenreCellStatus::None : GenreCellStatus::Include;
        Refresh();
    }

    void GenrePickerCell::ExcludeGenre() {
        if (genre == nullptr) {
            return;
        }
        genre->status = GenreCellStatus::Exclude;
        Refresh();
    }
}  // namespace BetterSongSearch::UI::Modals
