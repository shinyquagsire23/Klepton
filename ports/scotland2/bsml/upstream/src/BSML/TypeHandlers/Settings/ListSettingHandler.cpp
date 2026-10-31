#include "BSML/TypeHandlers/Settings/ListSettingHandler.hpp"

namespace BSML {
    static ListSettingHandler listSettingHandler{};

    ListSettingHandler::Base::PropMap ListSettingHandler::get_props() const {
        return {
            {"options", {"options", "choices"}}
        };
    }

    ListSettingHandler::Base::SetterMap ListSettingHandler::get_setters() const {
        return {};
    }

    void ListSettingHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        auto component = reinterpret_cast<ListSetting*>(componentType.component);
        auto& data = componentType.data;

        auto optionsItr = data.find("options");
        if (optionsItr != data.end() && !optionsItr->second.empty()) {
            auto val = parserParams.TryGetValue(optionsItr->second);
            if (val) component->values = val->GetValue<List<System::Object*>*>();
            else ERROR("Value '{}' could not be found", optionsItr->second);
        }

        if (!component->values || component->values->get_Count() == 0) {
            ERROR("Did not give options for list setting! this is required!");
        }

        Base::HandleType(componentType, parserParams);
    }
}
