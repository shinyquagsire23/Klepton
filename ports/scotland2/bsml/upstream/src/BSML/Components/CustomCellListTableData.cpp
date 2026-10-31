#include "BSML/Components/CustomCellListTableData.hpp"
#include "BSML.hpp"

#include "HMUI/Touchable.hpp"
DEFINE_TYPE(BSML, CustomCellListTableData);

using namespace UnityEngine;

namespace BSML {
    void CustomCellListTableData::ctor() {
        reuseIdentifier = "BSMLCustomCellListTableData";
        cellSize = 8.5f;
        clickableCells = true;
        // this way if you make a list like <custom-cell-list data="someField"/> it'll still show cells even if you don't give it a cell template
        bsmlString = "<bg bg='round-rect-panel'><text text='Default Cell'/></bg>";
    }

    float CustomCellListTableData::CellSize() {
        return cellSize;
    }

    int CustomCellListTableData::NumberOfCells() {
        return data ? data->get_Count() : 0;
    }

    HMUI::TableCell* CustomCellListTableData::CellForIdx(HMUI::TableView* tableView, int idx) {
        if (data->get_Count() < idx) return nullptr;
        auto cell = tableView->DequeueReusableCellForIdentifier(reuseIdentifier).try_cast<BSML::CustomCellTableCell>().value_or(nullptr);
        auto cellData = data[idx];
        if (!cell) {
            cell = NewCellForData(cellData);
        } else {
            cell->dataObject = cellData;
            cell->Reused();
        }

        cell->set_reuseIdentifier(reuseIdentifier);
        cell->set_interactable(clickableCells);

        return cell;
    }

    BSML::CustomCellTableCell* CustomCellListTableData::NewCellForData(System::Object* data) {
        auto cellGo = GameObject::New_ctor("BSMLCustomCellTableCell");
        auto cell = cellGo->AddComponent<BSML::CustomCellTableCell*>();

        if (clickableCells) {
            cellGo->AddComponent<HMUI::Touchable*>();
            cell->set_interactable(true);
        }

        auto parser = parse_and_construct(bsmlString, cellGo->get_transform(), cell);
        cell->parserParams = parser->parserParams;
        cell->dataObject = data;
        cell->SetupPostParse(*parser->parserParams);

        return cell;
    }

    HMUI::TableView::IDataSource* CustomCellListTableData::i_IDataSource() { return reinterpret_cast<HMUI::TableView::IDataSource*>(this); }
}
