#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "HMUI/TextSegmentedControl.hpp"
#include "HMUI/SegmentedControl.hpp"
#include "Tab.hpp"
#include "../Parsing/BSMLParserParams.hpp"

DECLARE_CLASS_CODEGEN(BSML, TabSelector, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(HMUI::TextSegmentedControl*, textSegmentedControl);
    DECLARE_INSTANCE_FIELD(StringW, tabTag);
    DECLARE_INSTANCE_FIELD(StringW, leftButtonTag);
    DECLARE_INSTANCE_FIELD(StringW, rightButtonTag);
    DECLARE_INSTANCE_FIELD(ListW<Tab*>, tabs);
    DECLARE_INSTANCE_FIELD(ListW<Tab*>, visibleTabs);
    DECLARE_INSTANCE_FIELD(int, pageCount);
    DECLARE_INSTANCE_FIELD(int, currentPage);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, leftButton);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, rightButton);
    DECLARE_INSTANCE_FIELD(int, lastClickedPage);
    DECLARE_INSTANCE_FIELD(int, lastClickedIndex);
    DECLARE_INSTANCE_FIELD(bool, shouldRefresh);

    DECLARE_INSTANCE_METHOD(int, get_page);
    DECLARE_INSTANCE_METHOD(int, get_pageCount);
    DECLARE_INSTANCE_METHOD(void, set_pageCount, int value);

    DECLARE_INSTANCE_METHOD(void, TabSelected, HMUI::SegmentedControl* segmentedControl, int index);
    DECLARE_INSTANCE_METHOD(void, Refresh);
    DECLARE_INSTANCE_METHOD(void, SetSegmentedControlTexts, ListW<Tab*> tabs);
    DECLARE_INSTANCE_METHOD(void, PageLeft);
    DECLARE_INSTANCE_METHOD(void, PageRight);
    DECLARE_INSTANCE_METHOD(void, OnEnable);

    DECLARE_CTOR(ctor);

    public:
        void Setup(const BSMLParserParams& parserParams);
};