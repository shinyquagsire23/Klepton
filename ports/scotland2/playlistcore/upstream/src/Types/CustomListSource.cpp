#include "CustomTypes/CustomListSource.hpp"

#include "Main.hpp"
#include "bsml/shared/BSML-Lite.hpp"
#include "bsml/shared/BSML/Components/TableView.hpp"

DEFINE_TYPE(PlaylistCore, CustomTableCell);
DEFINE_TYPE(PlaylistCore, CustomListSource);

using namespace PlaylistCore;
using namespace BSML;

// copied from questui
void CustomListSource::ctor() {
    INVOKE_CTOR();
    expandCell = false;
    tableView = nullptr;
    static ConstString reuseName("PlaylistCoreListCell");
    reuseIdentifier = reuseName;
}

void CustomListSource::dtor() {
    this->~CustomListSource();
    Finalize();
}

HMUI::TableCell* CustomListSource::CellForIdx(HMUI::TableView* tableView, int idx) {
    // kinda needed
    if (!type) {
        LOG_ERROR("Type not supplied to list source");
        return nullptr;
    }
    // check for available reusable cells
    CustomTableCell* reusableCell = (CustomTableCell*) tableView->DequeueReusableCellForIdentifier(reuseIdentifier).unsafePtr();
    if (!reusableCell) {
        // create a new cell
        static ConstString name("CustomCellGameObject");
        auto cellObject = UnityEngine::GameObject::New_ctor(name);
        auto rectTransform = cellObject->AddComponent<UnityEngine::RectTransform*>();
        rectTransform->set_sizeDelta({15, 15});
        reusableCell = (CustomTableCell*) cellObject->AddComponent(type).ptr();
        reusableCell->set_reuseIdentifier(reuseIdentifier);
        reusableCell->init(getSprite(idx), getText(idx));
    } else {
        reusableCell->setSprite(getSprite(idx));
        reusableCell->setText(getText(idx));
    }
    return (HMUI::TableCell*) reusableCell;
}

float CustomListSource::CellSize() {
    return 15;
}

int CustomListSource::NumberOfCells() {
    return std::max(sprites.size(), texts.size());
}

void CustomListSource::setType(System::Type* cellType) {
    type = cellType;
}

void CustomListSource::addSprites(std::vector<UnityEngine::Sprite*> newSprites) {
    sprites.reserve(sprites.size() + newSprites.size());
    sprites.insert(sprites.end(), newSprites.begin(), newSprites.end());
}

void CustomListSource::replaceSprites(std::vector<UnityEngine::Sprite*> newSprites) {
    clearSprites();
    addSprites(newSprites);
}

void CustomListSource::clearSprites() {
    sprites.clear();
}

void CustomListSource::addTexts(std::vector<std::string> newTexts) {
    texts.reserve(texts.size() + newTexts.size());
    texts.insert(texts.end(), newTexts.begin(), newTexts.end());
}

void CustomListSource::replaceTexts(std::vector<std::string> newTexts) {
    clearTexts();
    addTexts(newTexts);
}

void CustomListSource::clearTexts() {
    texts.clear();
}

void CustomListSource::clear() {
    clearSprites();
    clearTexts();
}

UnityEngine::Sprite* CustomListSource::getSprite(int index) {
    if (index < 0 || index >= sprites.size())
        return nullptr;
    return sprites[index];
}

std::string CustomListSource::getText(int index) {
    if (index < 0 || index >= texts.size())
        return "";
    return texts[index];
}

// static scroll methods
void CustomListSource::ScrollListLeft(CustomListSource* list, int numCells) {
    // get table view as bsml table view
    auto tableView = reinterpret_cast<BSML::TableView*>(list->tableView);
    // both assume the table is vertical
    // int idx = tableView->get_scrolledRow();
    // idx -= tableView->get_scrollDistance();
    int idx = std::min(
        (int) (tableView->get_contentTransform()->get_anchoredPosition().x / tableView->get_cellSize()) * -1, tableView->get_numberOfCells() - 1
    );
    idx -= numCells;
    idx = std::max(idx, 0);
    tableView->ScrollToCellWithIdx(idx, HMUI::TableView::ScrollPositionType::Beginning, true);
}

void CustomListSource::ScrollListRight(CustomListSource* list, int numCells) {
    // get table view as bsml table view
    auto tableView = reinterpret_cast<BSML::TableView*>(list->tableView);
    // both assume the table is vertical
    // int idx = tableView->get_scrolledRow();
    // idx += tableView->get_scrollDistance();
    int idx = std::min(
        (int) (tableView->get_contentTransform()->get_anchoredPosition().x / tableView->get_cellSize()) * -1, tableView->get_numberOfCells() - 1
    );
    idx += numCells;
    int max = tableView->get_dataSource()->NumberOfCells();
    idx = std::min(idx, max - 1);
    tableView->ScrollToCellWithIdx(idx, HMUI::TableView::ScrollPositionType::Beginning, true);
}

void CustomTableCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    refreshVisuals();
}

void CustomTableCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    refreshVisuals();
}
