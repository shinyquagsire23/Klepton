#include "hooking.hpp"
#include "GlobalNamespace/LevelListTableCell.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "TMPro/TMP_Text.hpp"

MAKE_AUTO_HOOK_MATCH(LevelListTableCell_RefreshVisuals, &GlobalNamespace::LevelListTableCell::RefreshVisuals, void, GlobalNamespace::LevelListTableCell* self) {
    LevelListTableCell_RefreshVisuals(self);
    auto rectTransform = self->_songNameText->rectTransform;
    auto offsetMin = rectTransform->offsetMin;
    auto offsetMax = rectTransform->offsetMax;
    offsetMax.y = offsetMin.y + 6.5f; // by default sizeDelta.y is 5.74 which is _slightly_ too small
    rectTransform->offsetMax = offsetMax;
}
