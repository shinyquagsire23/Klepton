#include "BSML/Macros/ReparentMacro.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "StringParseHelper.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<ReparentMacro> reparentMacroParser({"macro.reparent"});

    BSMLMacro::PropMap ReparentMacro::get_props() const {
        return {
            {"transform", {"transform"}},
            {"transforms", {"transforms"}}
        };
    }

    void ReparentMacro::Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const {
        INFO("Executing ashost macro");
        auto transformItr = data.find("transform");
        if (transformItr != data.end()) {
            auto value = parserParams.TryGetValue(transformItr->second);
            if (value) {
                auto transform = value->GetValue<UnityEngine::Transform*>();
                if (transform) {
                    transform->SetParent(parent, false);
                }
            }
        }

        auto transformsItr = data.find("transforms");
        if (transformsItr != data.end()) {
            auto value = parserParams.TryGetValue(transformsItr->second);
            if (value) {
                ListW<UnityEngine::Transform*> transforms = value->GetValue<List<UnityEngine::Transform*>*>();
                if (transforms) {
                    for (auto t : transforms) {
                        t->SetParent(parent, false);
                    }
                }
            }
        }
    }
}
