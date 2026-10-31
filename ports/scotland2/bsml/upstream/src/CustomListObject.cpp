#include "CustomListObject.hpp"
#include "logging.hpp"

#include "UnityEngine/GameObject.hpp"

DEFINE_TYPE(BSML, CustomListObject);

namespace BSML {
    void CustomListObject::Setup(CustomCellTableCell* cell) {
        this->cell = cell;
        INFO("Setup!");
        auto objs = cell->parserParams->GetObjectsWithTag("cellText");
        if (!objs.empty()) cellText = objs[0]->GetComponent<TMPro::TextMeshProUGUI*>();
    }

    void CustomListObject::Reused(CustomCellTableCell* cell) {
        this->cell = cell;
        INFO("Reused!");
        auto objs = cell->parserParams->GetObjectsWithTag("cellText");
        if (!objs.empty()) cellText = objs[0]->GetComponent<TMPro::TextMeshProUGUI*>();
    }

    void CustomListObject::Select(CustomCellTableCell* cell) {
        this->cell = cell;
        INFO("Select!");
        if (!cellText) return;
        cellText->set_text("Got Selected!");
    }
}