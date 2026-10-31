#include "BSML/Parsing/BSMLParser.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLNodeParser.hpp"
#include "BSML/TypeHandlers/TypeHandler.hpp"
#include "logging.hpp"

namespace BSML {
    BSMLParser::BSMLParser() : root(new BSMLNode()), parserParams(std::make_shared<BSMLParserParams>()) {}
    BSMLParser::~BSMLParser() {
        delete root;
    }

    std::shared_ptr<BSMLParser> BSMLParser::parse(std::string_view str) {
        auto parser = std::make_shared<BSMLParser>();
        tinyxml2::XMLDocument doc;

        // we use the data & size here so even if the string view is not null terminated it works right/if it's a view into part of a string it doesn't read too far
        auto error = doc.Parse(str.data(), str.size());
        if (error != tinyxml2::XML_SUCCESS) {
            ERROR("Error parsing BSML document: {}", tinyxml2::XMLDocument::ErrorIDToName(error));
            DEBUG("Printing the XML file:\n{}", str);
            std::string validString = fmt::format("<vertical bg='round-rect-panel' pad='5' spacing='4' pref-height='20' vertical-fit='PreferredSize'><text font-size='6' text='ERROR PARSING BSML FILE' align='Center'/><text text='{}' align='Center'/></vertical>", tinyxml2::XMLDocument::ErrorIDToName(error));
            doc.Parse(validString.c_str(), validString.size());
        }

        auto root = parser->root;
        root->root = root;
        root->is_valid = true;

        auto handle = tinyxml2::XMLConstHandle(doc).FirstChildElement();
        auto element = handle.ToElement();
        while(element) {
            auto tag = BSMLDocParser::get_parser(element->Name())->parse(*element);
            root->AddChild(tag);

            handle = handle.NextSiblingElement();
            element = handle.ToElement();
        }

        return parser;
    }

    std::shared_ptr<BSMLParser> BSMLParser::parse_and_construct(std::string_view str, UnityEngine::Transform* parent, System::Object* host) {
        auto parser = parse(str);
        parser->Construct(parent, host);
        return parser;
    }

    void BSMLParser::Construct(UnityEngine::Transform* parent, System::Object* host) {
        parserParams->host = host;
        std::vector<ComponentTypeWithData*> components;

        INFO("Making values from host fields, props and methods");
        auto values = host ? BSMLValue::MakeValues(host) : std::map<std::string, BSML::BSMLValue *>{};
        for (auto& [key, value] : values) {
            INFO("Got value: {}", key);
            INFO("finfo: {}", (bool)value->fieldInfo);
            INFO("sinfo: {}", (bool)value->setterInfo);
            INFO("ginfo: {}", (bool)value->getterInfo);
            parserParams->AddValue(key, value);
        }

        auto actions = host ? BSMLAction::MakeActions(host) : std::map<std::string, BSML::BSMLAction*>{};
        for (auto& [key, action] : actions) {
            INFO("Got action: {}", key);
            parserParams->AddAction(key, action);
        }

        /// when making from a new "root" we skip the root itself
        root->HandleChildren(parent, *parserParams, components);

        for (auto comp : components) {
            comp->typeHandler->HandleTypeAfterParse(*comp, *parserParams);
        }

        for (auto component : components) {
            delete component;
        }

        components.clear();

        if (host) {
            auto postParseMinfo = il2cpp_functions::class_get_method_from_name(host->klass, "PostParse", 0);
            if (postParseMinfo) il2cpp_utils::RunMethod(host, postParseMinfo);
        }
    }

    std::shared_ptr<BSMLParserParams> BSMLParser::Construct(const BSMLNode* root, UnityEngine::Transform* parent, System::Object* host) {
        auto parserParams = std::make_shared<BSMLParserParams>();
        parserParams->host = host;
        std::vector<ComponentTypeWithData*> components;

        INFO("Making values from host fields, props and methods");
        auto values = host ? BSMLValue::MakeValues(host) : std::map<std::string, BSML::BSMLValue *>{};
        for (auto& [key, value] : values) {
            INFO("Got value: {}", key);
            parserParams->AddValue(key, value);
        }

        auto actions = host ? BSMLAction::MakeActions(host) : std::map<std::string, BSML::BSMLAction*>{};
        for (auto& [key, action] : actions) {
            INFO("Got action: {}", key);
            parserParams->AddAction(key, action);
        }

        root->HandleChildren(parent, *parserParams, components);
        for (auto comp : components) {
            comp->typeHandler->HandleTypeAfterParse(*comp, *parserParams);
        }

        for (auto component : components) {
            delete component;
        }
        components.clear();

        if (host) {
            auto postParseMinfo = il2cpp_functions::class_get_method_from_name(host->klass, "PostParse", 0);
            if (postParseMinfo) il2cpp_utils::RunMethod(host, postParseMinfo);
        }

        return parserParams;
    }

}
