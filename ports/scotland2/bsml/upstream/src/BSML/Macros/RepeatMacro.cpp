#include "BSML/Macros/RepeatMacro.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "StringParseHelper.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<RepeatMacro> repeatMacroParser({"macro.repeat"});

    BSMLMacro::PropMap RepeatMacro::get_props() const {
        return {
            {"count", {"i", "count", "number"}},
            {"passTags", {"pass-back-tags"}}
        };
    }

    void RepeatMacro::Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const {
        INFO("Executing repeat macro");
        auto host = parserParams.get_host();
        auto countItr = data.find("count");
        if (countItr != data.end()) {
            int count = StringParseHelper(countItr->second);
            
            bool passTags = false;
            auto passTagsItr = data.find("passTags");
            if (passTagsItr != data.end()) passTags = StringParseHelper(passTagsItr->second);


            for (int i = 0; i < count; i++) {
                auto params = BSMLParser::Construct(this, parent, host);
                if (passTags)
                    params->PassTaggedObjects(parserParams);
            }
        }
    }
}