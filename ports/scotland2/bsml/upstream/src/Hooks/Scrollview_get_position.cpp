#include "hooking.hpp"

#include "HMUI/ScrollView.hpp"

MAKE_AUTO_HOOK_MATCH(ScrollView_get_position, &HMUI::ScrollView::get_position, float, HMUI::ScrollView* self) {
    // horizontal -> * (0 * 2) - 1 = -1
    // vertical ->   * (1 * 2) - 1 =  1
    return ScrollView_get_position(self) * (((self->_scrollViewDirection == HMUI::ScrollView::ScrollViewDirection::Vertical) * 2) - 1);
}
