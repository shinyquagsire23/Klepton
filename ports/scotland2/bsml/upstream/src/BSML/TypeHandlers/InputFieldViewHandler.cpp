#include "BSML/TypeHandlers/InputFieldViewHandler.hpp"

#include "UnityEngine/Events/UnityAction_1.hpp"

namespace BSML {
    static InputFieldViewHandler inputFieldViewHandler{};

    InputFieldViewHandler::Base::PropMap InputFieldViewHandler::get_props() const {
        return {
            {"onChange", { "on-change" }},
            {"textLengthLimit", { "text-length-limit" }},
        };
    }

    InputFieldViewHandler::Base::SetterMap InputFieldViewHandler::get_setters() const {
        return {
            {"textLengthLimit", [](auto component, auto value){ component->_textLengthLimit = value; }}
        };
    }

    void InputFieldViewHandler::HandleType(const BSML::ComponentTypeWithData& componentType, BSML::BSMLParserParams& parserParams) {
        Base::HandleType(componentType, parserParams);

        auto fieldView = reinterpret_cast<HMUI::InputFieldView*>(componentType.component);
        auto& data = componentType.data;

        auto onChangeItr = data.find("onChange");
        if (onChangeItr != data.end() && !onChangeItr->second.empty()) {
            auto action = parserParams.TryGetAction(onChangeItr->second);
            if (action) {
                // this works absolutely different than any of the other BSML input methods
                // Do I care? no.
                fieldView->onValueChanged->AddListener(
                    custom_types::MakeDelegate<UnityEngine::Events::UnityAction_1<UnityW<HMUI::InputFieldView>>*>(
                        std::function<void(HMUI::InputFieldView*)>(
                            [onValueChange = action->GetFunction<StringW>()](auto fieldView){
                                onValueChange(fieldView->get_text());
                            }
                        )
                    )
                );
            } else {
                ERROR("Action '{}' could not be found", onChangeItr->second);
            }
        }
    }
}
