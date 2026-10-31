#include "BSML/Macros/AsHostMacro.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "StringParseHelper.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<AsHostMacro> asHostMacroParser({"macro.as-host"});

    BSMLMacro::PropMap AsHostMacro::get_props() const {
        return {
            {"host", {"host"}},
        };
    }

    void AsHostMacro::Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const {
        INFO("Executing ashost macro");
        auto hostItr = data.find("host");
        if (hostItr != data.end()) {
            int count = StringParseHelper(hostItr->second);
            auto host = parserParams.TryGetValue(hostItr->second);
            auto currentHost = host ? host->GetValue() : nullptr;
            if (currentHost) {
                BSMLParser::Construct(this, parent, currentHost);
            }
        }
    }
}