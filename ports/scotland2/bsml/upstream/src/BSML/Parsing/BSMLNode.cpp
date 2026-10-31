#include "BSML/Parsing/BSMLNode.hpp"
#include "BSML/ComponentTypeWithData.hpp"
#include "BSML/Parsing/BSMLParserParams.hpp"
#include "logging.hpp"

namespace BSML {
    BSMLNode::BSMLNode() : is_valid(false), children({}) {
        #if MAKE_DOCS
        nodeType = NodeType::None;
        #endif
    }
    
    BSMLNode::~BSMLNode() {
        for (auto child : children) {
            delete child;
        }
        children.clear();
    }
    bool BSMLNode::valid() const {
        return is_valid;
    }

    void BSMLNode::AddChild(BSMLNode* child) {
        child->root = root;
        child->parent = this;
        children.emplace_back(child);
    }

    void BSMLNode::parse(const tinyxml2::XMLElement& elem) {
        INFO("Parsing Node: {}", elem.Name());
        is_valid = true;

        for (const tinyxml2::XMLAttribute* a = elem.FirstAttribute(); a; a = a->Next()) {
            attributes[a->Name()] = a->Value();
        }
    }

    void BSMLNode::Handle(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& components) const {}

    void BSMLNode::HandleChildren(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& components) const {
        for (auto child : children) {
            child->Handle(parent, parserParams, components);
        }
    }
}