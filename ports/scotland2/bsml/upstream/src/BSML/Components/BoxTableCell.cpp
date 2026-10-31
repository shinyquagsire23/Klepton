#include "BSML/Components/BoxTableCell.hpp"

#include "UnityEngine/Transform.hpp"
#include "UnityEngine/GameObject.hpp"

DEFINE_TYPE(BSML, BoxTableCell);

using namespace UnityEngine;

namespace BSML {
    void BoxTableCell::ctor() {
        selectedColor0 = {0, 0.75f, 1, 1};
        selectedColor1 = {0, 0.75f, 1, 0};
        highlightedColor0 = {0, 0.75f, 1, 1};
        highlightedColor1 = {0, 0.75f, 1, 0};
    }

    BoxTableCell* BoxTableCell::Create(GlobalNamespace::AnnotatedBeatmapLevelCollectionCell* prefab) {
        auto cell = Object::Instantiate(prefab);
        auto coverImage = cell->_coverImage;
        auto selectionImage = cell->_selectionImage;

        auto imageTransform = coverImage->get_transform();
        int childCount = imageTransform->get_childCount();
        for (int i = 0; i < childCount; i++) {
            Object::Destroy(imageTransform->GetChild(0)->get_gameObject());
        }

        auto cellObject = cell->get_gameObject();
        Object::Destroy(cell);
        auto boxTableCell = cellObject->AddComponent<BoxTableCell*>();
        boxTableCell->SetComponents(coverImage, selectionImage);
        return boxTableCell;
    }

    void BoxTableCell::SetComponents(HMUI::ImageView* coverImage, HMUI::ImageView* selectionImage) {
        this->coverImage = coverImage;
        this->selectionImage = selectionImage;
    }

    void BoxTableCell::SetData(UnityEngine::Sprite* coverSprite) {
        if (coverSprite) {
            coverImage->set_sprite(coverSprite);
        }
    }

    void BoxTableCell::RefreshVisuals() {
        if (get_selected() || get_highlighted()) {
            selectionImage->set_enabled(true);
            selectionImage->set_color0( get_highlighted() ? highlightedColor0 : selectedColor0);
            selectionImage->set_color1( get_highlighted() ? highlightedColor1 : selectedColor1);
        } else {
            selectionImage->set_enabled(false);
        }
    }

    void BoxTableCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        RefreshVisuals();
    }

    void BoxTableCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        RefreshVisuals();
    }
}
