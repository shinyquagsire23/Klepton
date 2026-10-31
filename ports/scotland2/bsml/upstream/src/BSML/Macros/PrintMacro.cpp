#include "BSML/Macros/PrintMacro.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "StringParseHelper.hpp"
#include "logging.hpp"

namespace BSML {
    static BSMLNodeParser<PrintMacro> printMacroParser({"macro.print", "macro.log"});

    BSMLMacro::PropMap PrintMacro::get_props() const {
        return {
            {"text", {"text", "string"}}
        };
    }

    void PrintMacro::Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const {
        INFO("Executing print macro");
        auto textItr = data.find("text");
        if (textItr != data.end()) {
            INFO("Printing from BSML macro: {}", textItr->second);
        }
    }
}