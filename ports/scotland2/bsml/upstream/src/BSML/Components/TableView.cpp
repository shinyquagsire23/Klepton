#include "BSML/Components/TableView.hpp"

#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/RectTransform.hpp"

#include "HMUI/FloatRectOffset.hpp"

DEFINE_TYPE(BSML, TableView);

void BSML::TableView::ctor() {
    INVOKE_BASE_CTOR(classof(HMUI::TableView*));
    _padding = HMUI::FloatRectOffset::New_ctor();
}

void BSML::TableView::ReloadData() {
    if (get_tableType() == TableType::Horizontal) {
        contentTransform->set_anchorMin({0, 0});
        contentTransform->set_anchorMax({0, 1});
    }

    static auto methodInfo = il2cpp_functions::class_get_method_from_name(classof(HMUI::TableView*), "ReloadData", 0);
    if (methodInfo) il2cpp_utils::RunMethod(this, methodInfo);
}

void BSML::TableView::DidSelectCellWithIdx(int idx) {
    static auto methodInfo = il2cpp_functions::class_get_method_from_name(classof(HMUI::TableView*), "DidSelectCellWithIdx", 1);
    if (methodInfo) il2cpp_utils::RunMethod(this, methodInfo, idx);

    // TODO: possibly use the klass for the tableData to call a callback on that instead of registering things?
}
