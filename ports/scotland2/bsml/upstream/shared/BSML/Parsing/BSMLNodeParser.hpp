#pragma once

#include "../../_config.h"
#include "tinyxml2/shared/tinyxml2.h"
#include <map>
#include <string>
#include <functional>
#include <vector>

namespace BSML {
    class BSMLNode;
    class BSMLDocParser;

    class BSML_EXPORT BSMLNodeParserBase {
        public:
            BSMLNodeParserBase(const std::vector<std::string>& aliases);
            virtual ~BSMLNodeParserBase();

            virtual BSMLNode* newNode() const = 0;
            virtual BSMLNode* parse(const tinyxml2::XMLElement& elem) const;
            void AddChild(BSMLNode* child);
        protected:
            void ParseChildren(const tinyxml2::XMLElement& elem, BSMLNode* parentNode) const;

            friend class BSMLDocParser;
            const std::vector<std::string> aliases;
    };

    template<typename T = BSMLNode>
    requires(std::is_base_of_v<::BSML::BSMLNode, T> || std::is_same_v<::BSML::BSMLNode, T>)
    class BSML_EXPORT BSMLNodeParser : BSMLNodeParserBase {
        public:
            BSMLNodeParser(const std::vector<std::string>& aliases = {"bsml"}) : BSMLNodeParserBase(aliases) {}
            virtual BSMLNode* newNode() const override { return new T(); }
    };
}
