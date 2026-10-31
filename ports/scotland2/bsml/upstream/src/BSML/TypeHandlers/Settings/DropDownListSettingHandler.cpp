#include "BSML/TypeHandlers/Settings/DropDownListSettingHandler.hpp"

namespace BSML {
    static DropdownListSettingHandler dropdownListSettingHandler{};

    DropdownListSettingHandler::Base::PropMap DropdownListSettingHandler::get_props() const {
        return {
            {"options", {"options", "choices"}}
        };
    }

    DropdownListSettingHandler::Base::SetterMap DropdownListSettingHandler::get_setters() const {
        return {};
    }

    void DropdownListSettingHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        auto component = reinterpret_cast<DropdownListSetting*>(componentType.component);
        auto& data = componentType.data;

        auto optionsItr = data.find("options");
        if (optionsItr != data.end() && !optionsItr->second.empty()) {
            auto val = parserParams.TryGetValue(optionsItr->second);
            if (val) component->values = val->GetValue<List<System::Object*>*>();
        }

        if (!component->values || component->values->get_Count() == 0) {
            ERROR("Did not give options for dropdown list! this is required!");
        }

        Base::HandleType(componentType, parserParams);
    }
}
