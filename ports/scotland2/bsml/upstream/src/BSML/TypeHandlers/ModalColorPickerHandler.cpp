#include "BSML/TypeHandlers/ModalColorPickerHandler.hpp"

namespace BSML {
    ModalColorPickerHandler modalColorPickerHandler{};

    ModalColorPickerHandler::Base::PropMap ModalColorPickerHandler::get_props() const {
        return {
            {"value", {"value"}},
            {"onCancel", {"on-cancel"}},
            {"onDone", {"on-done"}},
            {"onChange", {"color-change"}}
        };
    }

    ModalColorPickerHandler::Base::SetterMap ModalColorPickerHandler::get_setters() const {
        return {};
    }

    void ModalColorPickerHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        auto colorPicker = reinterpret_cast<ModalColorPicker*>(componentType.component);
        auto host = parserParams.get_host();
        auto& data = componentType.data;

        colorPicker->host = host;
        auto valueItr = data.find("value");
        if (valueItr != data.end()) {
            auto val = parserParams.TryGetValue(valueItr->second);
            auto genericSetting = colorPicker->genericSetting;
            if (val) {
                genericSetting->host = val->host;
                genericSetting->valueInfo = val->fieldInfo;
                genericSetting->getterInfo = val->getterInfo;
                genericSetting->setterInfo = val->setterInfo;
            }
        }

        Base::HandleType(componentType, parserParams);
    }

    void ModalColorPickerHandler::HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        Base::HandleTypeAfterParse(componentType, parserParams);
        auto colorPicker = reinterpret_cast<ModalColorPicker*>(componentType.component);
        auto& data = componentType.data;

        auto onCancelItr = data.find("onCancel");
        if (onCancelItr != data.end() && !onCancelItr->second.empty()) {
            auto action = parserParams.TryGetAction(onCancelItr->second);
            if (action) colorPicker->onCancelInfo = action->methodInfo;
            else ERROR("Action '{}' could not be found", onCancelItr->second);
        }

        auto onDoneItr = data.find("onDone");
        if (onDoneItr != data.end() && !onDoneItr->second.empty()) {
            auto action = parserParams.TryGetAction(onDoneItr->second);
            if (action) colorPicker->onCancelInfo = action->methodInfo;
            else ERROR("Action '{}' could not be found", onDoneItr->second);
        }

        auto onChangeItr = data.find("onChange");
        if (onChangeItr != data.end() && !onChangeItr->second.empty()) {
            auto action = parserParams.TryGetAction(onChangeItr->second);
            if (action) colorPicker->colorChangeInfo = action->methodInfo;
            else ERROR("Action '{}' could not be found", onChangeItr->second);
        }
    }
}