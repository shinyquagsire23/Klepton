#include "BSML/Parsing/BSMLNodeParser.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLNode.hpp"
#include "logging.hpp"


namespace BSML {
    BSMLNodeParserBase::BSMLNodeParserBase(const std::vector<std::string>& aliases) : aliases(aliases) {
        BSMLDocParser::RegisterTag(this);
    }

    BSMLNodeParserBase::~BSMLNodeParserBase() {
        BSMLDocParser::UnRegisterTag(this);
    }

    BSMLNode* BSMLNodeParserBase::parse(const tinyxml2::XMLElement& elem) const {
        auto tag = newNode();
        tag->parse(elem);
        
        ParseChildren(elem, tag);
        return tag;
    }

    void BSMLNodeParserBase::ParseChildren(const tinyxml2::XMLElement& elem, BSMLNode* parentNode) const {
        auto handle = tinyxml2::XMLConstHandle(elem).FirstChildElement();
        for (
            const tinyxml2::XMLElement* element = nullptr;
            (element = handle.ToElement()) != nullptr;
            handle = handle.NextSiblingElement()
        ) {
            parentNode->AddChild(BSMLDocParser::get_parser(element->Name())->parse(*element));
        }
    }
}