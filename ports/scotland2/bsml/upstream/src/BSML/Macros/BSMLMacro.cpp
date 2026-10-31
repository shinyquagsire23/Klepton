#include "BSML/Macros/BSMLMacro.hpp"

#include "BSML/Parsing/BSMLParserParams.hpp"

extern std::string BSMLValueToString(BSML::BSMLValue* v, Il2CppTypeEnum type);

namespace BSML {
    BSMLMacro::BSMLMacro() : BSMLNode() {
        #if MAKE_DOCS
        nodeType = NodeType::Macro;
        #endif
    };

    const BSMLMacro::PropMap& BSMLMacro::get_cachedProps() const {
        if (cachedProps.empty())
            const_cast<BSMLMacro*>(this)->cachedProps = get_props();
        return cachedProps;
    }

    void BSMLMacro::Handle(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& componentInfo) const {
        std::map<std::string, std::string> data;

        // for each property in this macro
        for (const auto& [key, aliases] : get_cachedProps()) {
            // try to find it's aliases in the data
            for (const auto& alias : aliases) {
                auto itr = attributes.find(alias);
                // not found -> continue;
                if (itr == attributes.end()) continue;
                // if value starts with ~ get value from parser params
                if (!itr->second.empty() && itr->second[0] == '~') {
                    // if start with ~ look the value up in the parserParams values
                    auto key = itr->second.substr(1);
                    auto v = parserParams.TryGetValue(key);
                    if (v) {
                        if (v->fieldInfo) {
                            data[key] = BSMLValueToString(v, v->fieldInfo->type->type);
                        } else if (v->getterInfo) {
                            data[key] = BSMLValueToString(v, v->getterInfo->return_type->type);
                        }
                        break;
                    }
                    // if the value was not found we assign the actual name to the prop so it can at least try to be used
                } 
                    data[key] = itr->second;
                    break;
                }
        }

        Execute(parent, data, parserParams, componentInfo);
    }
}