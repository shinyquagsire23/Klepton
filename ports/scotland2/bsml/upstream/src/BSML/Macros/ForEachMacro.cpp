#include "BSML/Macros/ForEachMacro.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "StringParseHelper.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<ForEachMacro> foreachMacroParser({"macro.for-each"});

    BSMLMacro::PropMap ForEachMacro::get_props() const {
        return {
            {"hosts", {"hosts", "items"}},
            {"passTags", {"pass-back-tags"}}
        };
    }

    void ForEachMacro::Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const {
        INFO("Executing foreach macro");
        auto hostsItr = data.find("hosts");
        if (hostsItr != data.end()) {
            auto value = parserParams.TryGetValue(hostsItr->second);
            if (value) {
                ListW<System::Object*> hosts = value->GetValue<List<System::Object*>*>();
                if (hosts) {
                    bool passTags = false;
                    auto passTagsItr = data.find("passTags");
                    if (passTagsItr != data.end()) passTags = StringParseHelper(passTagsItr->second);
                    for (auto host : hosts) {
                        auto params = BSMLParser::Construct(this, parent, host);
                        if (passTags)
                            params->PassTaggedObjects(parserParams);
                    }
                } else {
                    ERROR("Did not find a valid hosts list for foreach macro!");
                    ERROR("This means BSML could not find value '{0}' or method 'get_{0}' in wrapped values", hostsItr->second);
                }
            }
        }
    }
}
