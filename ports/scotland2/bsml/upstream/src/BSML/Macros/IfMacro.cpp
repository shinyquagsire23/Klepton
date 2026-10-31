#include "BSML/Macros/IfMacro.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "StringParseHelper.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<IfMacro> ifMacroParser({"macro.if"});

    BSMLMacro::PropMap IfMacro::get_props() const {
        return {
            {"value", {"bool", "value"}},
        };
    }

    void IfMacro::Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const {
        INFO("Executing if macro");
        auto valueItr = data.find("value");
        if (valueItr == data.end()) return;

        auto valueId = StringParseHelper(valueItr->second);
        bool notOperator = valueId.size() > 1 && valueId[0] == '!';
        if (notOperator) {
            valueId = StringParseHelper(valueId.substr(1));
        }

        INFO("Getting value '{}' from parser params", valueId);
        auto value = parserParams.TryGetValue(valueId);
        if (value) {
            INFO("Got value!");
            auto boolOpt = value->GetValueOpt<bool>();
            if (notOperator != boolOpt.value_or(notOperator)) {
                INFO("Handling Children");
                HandleChildren(parent, parserParams, componentInfo);
            }
        } else {
            ERROR("Could not find value {}", valueId);
        }
    }
}
