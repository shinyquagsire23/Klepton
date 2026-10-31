#pragma once

#include "../../_config.h"
#include "tinyxml2/shared/tinyxml2.h"
#include <string>
#include <vector>
#include <map>

#include "UnityEngine/Transform.hpp"
#include "BSMLNodeParser.hpp"

namespace BSML {
    class ComponentTypeWithData;
    class BSMLParserParams;
    class BSMLDocParser;
    class BSMLParser;
    class BSML_EXPORT BSMLNode {
        public:
            BSMLNode();
            ~BSMLNode();

            virtual void Handle(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& componentInfo) const;
            virtual void HandleChildren(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& componentInfo) const;
            bool valid() const;
            void AddChild(BSMLNode* child);

            friend class ::BSML::BSMLDocParser;
            friend class ::BSML::BSMLNodeParserBase;
            friend class ::BSML::BSMLParser;
        protected:

            bool is_valid = false;
            BSMLNode* root;
            BSMLNode* parent;
            std::vector<BSMLNode*> children;
            std::map<std::string, std::string> attributes = {};

            #if MAKE_DOCS
            enum class NodeType {
                None,
                Tag,
                Macro
            };
            NodeType nodeType;
            #endif

            virtual void parse(const tinyxml2::XMLElement& elem);
    };
}
