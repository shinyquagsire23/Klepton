#include "BSML/TypeHandlers/ButtonHandler.hpp"
#include "Helpers/delegates.hpp"
#include "logging.hpp"

#include "UnityEngine/Events/UnityAction.hpp"

#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

using namespace UnityEngine;
using namespace UnityEngine::Events;
using namespace UnityEngine::UI;

namespace BSML {
    static ButtonHandler buttonHandler;
    ButtonHandler::Base::PropMap ButtonHandler::get_props() const {
        return {
            {"onClick", {"on-click"}},
            {"clickEvent", {"click-event", "event-click"}}
        };
    }

    ButtonHandler::Base::SetterMap ButtonHandler::get_setters() const {
        return {};
    }

    void ButtonHandler::HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        Base::HandleType(componentType, parserParams);
        auto button = reinterpret_cast<Button*>(componentType.component);
        auto event = Button::ButtonClickedEvent::New_ctor();
        button->set_onClick(event);
    }

    void ButtonHandler::HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) {
        Base::HandleTypeAfterParse(componentType, parserParams);
        
        auto button = reinterpret_cast<Button*>(componentType.component);
        auto event = button->get_onClick();

        // it was a button!
        auto onClickItr = componentType.data.find("onClick");
        if (onClickItr != componentType.data.end() && !onClickItr->second.empty()) {
            auto action = parserParams.TryGetAction(onClickItr->second);
            if (action) {
                event->AddListener(action->GetUnityAction());
            }
        }

        auto clickEventItr = componentType.data.find("click-event");
        if (clickEventItr != componentType.data.end() && !clickEventItr->second.empty()) {
            auto parserEvent = parserParams.GetEvent(clickEventItr->second);
            auto action = MakeUnityAction([parserEvent](){
                if (!parserEvent.expired()) {
                    parserEvent.lock()->Invoke();
                } else {
                    ERROR("Event pointer expired, are you saving your parser params?");
                }
            });
            event->AddListener(action);
        }
    }
}