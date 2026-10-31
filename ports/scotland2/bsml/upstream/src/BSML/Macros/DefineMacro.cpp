#include "BSML/Macros/DefineMacro.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "BSML/Parsing/BSMLStringValue.hpp"
#include "StringParseHelper.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<DefineMacro> defineMacroParser({"macro.define"});

    BSMLMacro::PropMap DefineMacro::get_props() const {
        return {
            {"name", {"name", "id"}},
            {"value", {"value"}}
        };
    }

    void DefineMacro::Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const {
        INFO("Executing ashost macro");
        auto nameItr = data.find("name");
        if (nameItr == data.end()) {
            ERROR("Missing name property for macro");
            return;
        }
        auto valueItr = data.find("value");
        if (valueItr == data.end()) {
            ERROR("Missing value property for macro");
            return;
        }
        auto name = StringParseHelper(nameItr->second);
        auto value = StringParseHelper(valueItr->second);

        auto existingValue = parserParams.TryGetValue(name);
        if (existingValue) {
            existingValue->SetValue(StringW(value));
        } else {
            auto newValue = new BSMLStringValue();
            newValue->host = parserParams.get_host();
            newValue->name = name;
            newValue->SetValue(static_cast<System::Object*>(StringW(value).convert()));
            parserParams.AddValue(name, newValue);
        }
    }
}
