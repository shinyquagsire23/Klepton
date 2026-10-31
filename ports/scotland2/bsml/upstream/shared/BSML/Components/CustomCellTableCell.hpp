#pragma once

#include "custom-types/shared/macros.hpp"
#include "../Parsing/BSMLParserParams.hpp"
#include "UnityEngine/GameObject.hpp"
#include "HMUI/SelectableCell.hpp"
#include "HMUI/TableCell.hpp"

DECLARE_CLASS_CODEGEN(BSML, CustomCellTableCell, HMUI::TableCell) {
    DECLARE_INSTANCE_FIELD(System::Object*, dataObject);
    DECLARE_INSTANCE_FIELD(ListW<UnityEngine::GameObject*>, selectedTags);
    DECLARE_INSTANCE_FIELD(ListW<UnityEngine::GameObject*>, hoveredTags);
    DECLARE_INSTANCE_FIELD(ListW<UnityEngine::GameObject*>, neitherTags);

    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_INSTANCE_METHOD(void, RefreshVisuals);
    DECLARE_INSTANCE_METHOD(void, Reused);

    DECLARE_CTOR(ctor);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Finalize, &System::Object::Finalize);

    public:
        std::shared_ptr<BSMLParserParams> parserParams;
        void SetupPostParse(const BSMLParserParams& parserParams);
};