#include "hooking.hpp"

#include "HMUI/SegmentedControl.hpp"
#include "HMUI/SegmentedControlCell.hpp"
#include "System/Action_2.hpp"
#include "System/Action_1.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"


MAKE_AUTO_HOOK_ORIG_MATCH(SegmentedControl_HandleCellSelectionDidChange, &HMUI::SegmentedControl::HandleCellSelectionDidChange, void, HMUI::SegmentedControl* self, HMUI::SelectableCell* selectableCell, HMUI::SelectableCell::TransitionType transitionType, System::Object* changeOwner) {
    if (self->selectedCellNumber == -1) {
        auto segmentedCell = reinterpret_cast<HMUI::SegmentedControlCell*>(selectableCell);
        self->_selectedCellNumber = segmentedCell->cellNumber;
        if (self->didSelectCellEvent) {
            self->didSelectCellEvent->Invoke(self, segmentedCell->cellNumber);
        }

        System::Action_1<int>* value = nullptr;
        if (self->_callbacks->TryGetValue(segmentedCell->cellNumber, byref(value))) {
            if (value) value->Invoke(segmentedCell->cellNumber);
        }

    } else {
        SegmentedControl_HandleCellSelectionDidChange(self, selectableCell, transitionType, changeOwner);
    }
}
