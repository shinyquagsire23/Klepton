#include "hooking.hpp"

#include "HMUI/TableView.hpp"
#include "HMUI/ScrollView.hpp"

static int ClampIndex(HMUI::TableView* self, int idx)
{
    if (idx < 0)
    {
        return 0;
    }
    else if (idx >= self->numberOfCells)
    {
        return self->numberOfCells - 1;
    }
    else
    {
        return idx;
    }
}

float GetTargetScrollPosition(HMUI::TableView* self, float cellPosition, HMUI::TableView::ScrollPositionType scrollPositionType)
{
    using ScrollPositionType = HMUI::TableView::ScrollPositionType;
    switch(scrollPositionType)
    {
        case ScrollPositionType::Beginning: return cellPosition;
        case ScrollPositionType::Center: return cellPosition - ((self->scrollView->scrollPageSize - self->_cellSize) / 2);
        case ScrollPositionType::End: return cellPosition - (self->scrollView->scrollPageSize - self->_cellSize);
        default: return cellPosition;
    };
}

MAKE_AUTO_HOOK_MATCH(TableView_ScrollToCellWithIdx, &HMUI::TableView::ScrollToCellWithIdx, void, HMUI::TableView* self, int idx, HMUI::TableView::ScrollPositionType type, bool animated)
{
    idx = ClampIndex(self, idx);
    float position = self->GetCellPosition(idx);
    position = GetTargetScrollPosition(self, position, type);
    self->ScrollToPosition(position, animated);
}