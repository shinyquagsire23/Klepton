#include "BSML/TypeHandlers/IconSegmentedControlHandler.hpp"
#include "Helpers/delegates.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

namespace BSML {
    static IconSegmentedControlHandler textSegmentedControlHandler{};

    IconSegmentedControlHandler::Base::PropMap IconSegmentedControlHandler::get_props() const {
        return {
            {"selectCell", {"select-cell", "cell-select"}},
            {"data", {"contents", "data"}}
        };
    }

    IconSegmentedControlHandler::Base::SetterMap IconSegmentedControlHandler::get_setters() const {
        return {};
    }

    void IconSegmentedControlHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        auto textControl = reinterpret_cast<HMUI::IconSegmentedControl*>(componentType.component);
        auto& data = componentType.data;

        auto dataItr = data.find("data");
        if (dataItr != data.end() && !dataItr->second.empty()) {
            auto val = parserParams.TryGetValue(dataItr->second);
            auto iconData = val ? val->GetValue<::System::Collections::Generic::List_1<HMUI::IconSegmentedControl::DataItem*>*>() : nullptr;
            static auto dataKlass = classof(List<HMUI::IconSegmentedControl::DataItem*>*);
            if (iconData && il2cpp_functions::class_is_assignable_from(iconData->klass, dataKlass) && iconData->get_Count() > 0) {
                textControl->SetData(iconData->ToArray());
            } else if (iconData && !il2cpp_functions::class_is_assignable_from(iconData->klass, dataKlass)) {
                ERROR("{}::{} is not assignable from {}::{}", iconData->klass->namespaze, iconData->klass->name, dataKlass->namespaze, dataKlass->name);
            } else {
                ERROR("IconSegmentedControl needs to have at least 1 value!");
                ERROR("This means BSML could not find value '{0}'", dataItr->second);
            }
        }

        auto selectCellItr = data.find("selectCell");
        if (selectCellItr != data.end() && !selectCellItr->second.empty()) {
            auto action = parserParams.TryGetAction(selectCellItr->second);

            if (action) textControl->add_didSelectCellEvent(action->GetSystemAction<UnityW<HMUI::SegmentedControl>, int>());
            else ERROR("Action '{}' could not be found", selectCellItr->second);
        }

        Base::HandleType(componentType, parserParams);
    }
}
