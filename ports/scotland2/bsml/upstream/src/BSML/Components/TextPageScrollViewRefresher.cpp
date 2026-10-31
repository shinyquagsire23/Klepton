#include "BSML/Components/TextPageScrollViewRefresher.hpp"

#include "TMPro/TextMeshProUGUI.hpp"

DEFINE_TYPE(BSML, TextPageScrollViewRefresher);

namespace BSML {
    void TextPageScrollViewRefresher::OnEnable() {
        if (scrollView && scrollView->m_CachedPtr.m_value) {
            scrollView->SetText(scrollView->_text ? scrollView->_text->get_text() : "");
            scrollView->RefreshButtons();
        }

    }

    void TextPageScrollViewRefresher::OnRectTransformDimensionsChange() {
        if (scrollView && scrollView->m_CachedPtr.m_value) {
            scrollView->SetText(scrollView->_text ? scrollView->_text->get_text() : "");
            scrollView->RefreshButtons();
        }
    }
}
