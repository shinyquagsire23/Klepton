#include "BSML/Tags/BSMLTag.hpp"
#include "BSML/TypeHandlers/TypeHandler.hpp"
#include "BSML/Components/ExternalComponents.hpp"

#include "logging.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"

#include "System/Object.hpp"
#include <string.h>

UnityEngine::Component* GetExternalComponent(UnityEngine::GameObject* obj, BSML::ExternalComponents* externalComponents, System::Type* type) {
    UnityEngine::Component* result = nullptr;
    if (externalComponents) {
        result = externalComponents->GetByType(type);
    }
    return result ? result : static_cast<UnityEngine::Component*>(obj->GetComponent(type));
}

namespace BSML {
    static BSMLNodeParser<BSMLTag> bsmlNodeParser({"bsml"});
    BSMLTag::BSMLTag() : BSMLNode() {
        #if MAKE_DOCS
        nodeType = NodeType::Tag;
        #endif
    }

    void BSMLTag::Handle(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& componentInfo) const {
        // create object
        auto currentObject = CreateObject(parent);
        auto externalComponents = currentObject->GetComponent<ExternalComponents*>();

        std::vector<ComponentTypeWithData*> localComponentInfo = {};
        auto& typeHandlers = TypeHandlerBase::get_typeHandlers();
        // get the type handlers for the components on currentObject
        for (auto typeHandler : typeHandlers) {
            auto type = typeHandler->get_type();
            auto component = GetExternalComponent(currentObject, externalComponents, type);
            if (component)
            {
                INFO("Found component {}", type->FullNameOrDefault);
                auto componentTypeWithData = new ComponentTypeWithData();
                componentTypeWithData->typeHandler = typeHandler;
                componentTypeWithData->component = component;
                componentTypeWithData->data = ComponentTypeWithData::GetParameters(attributes, parserParams, typeHandler->get_cachedProps());
                localComponentInfo.emplace_back(componentTypeWithData);
            }
        }

        // handle type initially
        for (auto componentTypeWithData : localComponentInfo) {
            componentTypeWithData->typeHandler->HandleType(*componentTypeWithData, parserParams);
        }

        // check host for null & check id even given at all
        auto host = parserParams.host;
        if (host && !id.empty()) {
            // set the host field if we can
            auto fieldInfo = il2cpp_functions::class_get_field_from_name(parserParams.host->klass, id.c_str());
            if (fieldInfo) {
                auto fieldSystemType = reinterpret_cast<System::Type*>(il2cpp_utils::GetSystemType(il2cpp_functions::field_get_type(fieldInfo)));
                static auto gameObjectSystemType = csTypeOf(UnityEngine::GameObject*);
                if (gameObjectSystemType == fieldSystemType) {
                    // if the field is of type GameObject, set the field to the current object value
                    SetHostField(parserParams.host, currentObject);
                } else {
                    // if the field is not a GameObject, try to find the type of the field with the GetExternalComponent method, and set that on the field
                    auto component = GetExternalComponent(currentObject, externalComponents, fieldSystemType);
                    if (component) {
                        SetHostField(parserParams.host, component);
                    }
                }
            }
        }

        // add object to tags on parserParams
        if (!tags.empty()) {
            parserParams.AddObjectWithTags(currentObject, tags);
        }
        // handle children
        HandleChildren(currentObject->get_transform(), parserParams, componentInfo);

        // handle type after children
        for (auto componentTypeWithData : localComponentInfo) {
            componentTypeWithData->typeHandler->HandleTypeAfterChildren(*componentTypeWithData, parserParams);
        }

        // add all localComponentInfo to global one
        componentInfo.insert(componentInfo.begin(), localComponentInfo.begin(), localComponentInfo.end());
    }

    UnityEngine::GameObject* BSMLTag::CreateObject(UnityEngine::Transform* parent) const {
        return parent->get_gameObject();
    }

    void BSMLTag::SetHostField(System::Object* host, System::Object* value) const {
        if (!host || id.empty()) return;

        // get the field info with il2cpp reflection
        auto fieldInfo = il2cpp_functions::class_get_field_from_name(host->klass, id.c_str());

        if (fieldInfo) {
            // if value type is convertible to as field type, assign
            if (il2cpp_utils::IsConvertibleFrom(fieldInfo->type, &value->klass->byval_arg))
                il2cpp_functions::field_set_value(host, fieldInfo, value);
        }
    }

    #define GET_BSML_STRING(identifier, store)                      \
    {                                                               \
        const char* temp_##store = nullptr;                         \
        if (!elem.QueryStringAttribute(identifier, &temp_##store))  \
            store = std::string_view(temp_##store);                 \
    }

    void BSMLTag::parse(const tinyxml2::XMLElement& elem) {
        BSMLNode::parse(elem);
        is_valid = true;
        GET_BSML_STRING("id", id);

        std::string tagString;
        GET_BSML_STRING("tags", tagString);
        if (!tagString.empty()) {
            char* split = strtok(tagString.data(), ",");
            while (split) {
                tags.emplace_back(split);
                split = strtok(nullptr, ",");
            }
        }
    }
}
