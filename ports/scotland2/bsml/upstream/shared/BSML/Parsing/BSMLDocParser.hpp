#pragma once

#include "../../_config.h"
#include "BSMLParserParams.hpp"
#include "tinyxml2/shared/tinyxml2.h"
#include <map>
#include <string>

namespace BSML {
    class BSMLNode;
    class BSMLNodeParserBase;
    class BSML_EXPORT BSMLDocParser {
        public:
            static void RegisterTag(BSMLNodeParserBase* parser);
            static void UnRegisterTag(BSMLNodeParserBase* parser);

            static BSMLNodeParserBase* get_parser(std::string alias);
            #if MAKE_DOCS
            static void PrintDocs();
            #endif
        private:
            using TagNameToParser = std::map<std::string, BSML::BSMLNodeParserBase*>;
            static TagNameToParser tagNameToParser;
    };
}
