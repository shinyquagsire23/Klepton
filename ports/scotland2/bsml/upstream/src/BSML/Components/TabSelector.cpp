#include "BSML/Components/TabSelector.hpp"
#include "logging.hpp"

#include "Helpers/delegates.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Events/UnityAction.hpp"

DEFINE_TYPE(BSML, TabSelector);

namespace BSML {
    void TabSelector::ctor() {
        tabs = ListW<Tab*>::New();
        visibleTabs = ListW<Tab*>::New();
        pageCount = -1;
    }

    int TabSelector::get_page() {
        if (currentPage < 0) {
            currentPage = 0;
            return currentPage;
        }
        int maxPages = (visibleTabs->get_Count() - 1) / get_pageCount();
        if (currentPage > maxPages) {
            currentPage = maxPages;
            return currentPage;
        }
        return currentPage;
    }

    int TabSelector::get_pageCount() {
        return pageCount;
    }

    void TabSelector::set_pageCount(int value) {
        pageCount = value;
        if (tabs->get_Count() > 0) Refresh();
    }

    void TabSelector::Setup(const BSMLParserParams& parserParams) {
        DEBUG("TabSelector setup is ran!");
        if (System::String::IsNullOrEmpty(tabTag)) {
            ERROR("Tab selector must have tab-tag!");
            return;
        }

        tabs->Clear();

        for (auto go : parserParams.GetObjectsWithTag(tabTag)) {
            auto tab = go->GetComponent<Tab*>();
            tabs->Add(tab);
            tab->selector = this;
        }

        DEBUG("Got {} tabs!", tabs->get_Count());

        if (!System::String::IsNullOrEmpty(leftButtonTag)) {
            auto& leftButtons = parserParams.GetObjectsWithTag(leftButtonTag);
            if (!leftButtons.empty()) leftButton = leftButtons[0]->GetComponent<UnityEngine::UI::Button*>();
        }
        if (leftButton) {
            auto delegate = MakeUnityAction(std::bind(&TabSelector::PageLeft, this));
            leftButton->get_onClick()->AddListener(delegate);
        }
        if (!System::String::IsNullOrEmpty(rightButtonTag))  {
            auto& rightButtons = parserParams.GetObjectsWithTag(rightButtonTag);
            if (!rightButtons.empty()) rightButton = rightButtons[0]->GetComponent<UnityEngine::UI::Button*>();
        }
        if (rightButton) {
            auto delegate = MakeUnityAction(std::bind(&TabSelector::PageRight, this));
            rightButton->get_onClick()->AddListener(delegate);
        }

        DEBUG("left button: {}, right button: {}", leftButton != nullptr, rightButton != nullptr);
        Refresh();

        auto tabSelectedInfo = il2cpp_functions::class_get_method_from_name(this->klass, "TabSelected", 2);
        auto delegate = MakeSystemAction<UnityW<HMUI::SegmentedControl>, int>(this, tabSelectedInfo);
        textSegmentedControl->add_didSelectCellEvent(delegate);
        textSegmentedControl->SelectCellWithNumber(0);
        TabSelected(textSegmentedControl, 0);
    }

    void TabSelector::TabSelected(HMUI::SegmentedControl* segmentedControl, int index) {
        lastClickedPage = currentPage;
        lastClickedIndex = index;
        if (get_pageCount() != -1) index += get_pageCount() * currentPage;
        int visibleCount = 0;
        Tab* theTab = nullptr;
        for (auto tab : tabs) {
            tab->get_gameObject()->SetActive(false);
            if (tab->get_isVisible()) {
                if (index == visibleCount) theTab = tab;
                visibleCount++;
            }
        }

        // if our index is above or equal to the visible cells, return
        if (index >= visibleCount) return;
        if (theTab) theTab->get_gameObject()->SetActive(true);
    }

    void TabSelector::Refresh() {
        if (!get_isActiveAndEnabled()) {
            shouldRefresh = true;
            return;
        }

        DEBUG("TabSelector Refresh is ran!");

        shouldRefresh = false;
        visibleTabs->Clear();
        for (auto tab : tabs) if (tab->get_isVisible()) visibleTabs->Add(tab);

        if (get_pageCount() == -1) {
            SetSegmentedControlTexts(visibleTabs);
        } else {
            currentPage = get_page();
            ListW<Tab*> usableTabs = ListW<Tab*>::New();
            usableTabs->EnsureCapacity(get_pageCount());

            int start = get_pageCount() * currentPage;
            int end = std::min(start + pageCount, visibleTabs->get_Count());
            for (int i = start; i < end; i++) {
                usableTabs->Add(visibleTabs[i]);
            }

            SetSegmentedControlTexts(usableTabs);

            if (leftButton) leftButton->set_interactable(currentPage > 0);
            if (rightButton) rightButton->set_interactable(currentPage < ((visibleTabs->get_Count() - 1) / pageCount));

            TabSelected(nullptr, 0);
        }
    }

    void TabSelector::SetSegmentedControlTexts(ListW<Tab*> tabs) {
        // we have to use a list because Array does not implement IReadOnlyList
        auto texts = ListW<StringW>::New();
        texts->EnsureCapacity(tabs->get_Count());

        for (auto tab : tabs) {
            auto val = tab->get_tabKey();
            if (System::String::IsNullOrEmpty(val)) {
                val = tab->get_tabName();
            }
            texts->Add(val);

            DEBUG("tab Text added: {}", val);
        }

        textSegmentedControl->SetTexts(*texts, nullptr);
    }

    void TabSelector::PageLeft() {
        currentPage--;
        Refresh();
    }

    void TabSelector::PageRight() {
        currentPage++;
        Refresh();
    }

    void TabSelector::OnEnable() {
        if (shouldRefresh) Refresh();
    }
}
