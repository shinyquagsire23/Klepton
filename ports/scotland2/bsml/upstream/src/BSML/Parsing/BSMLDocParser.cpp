#include "BSML/Parsing/BSMLDocParser.hpp"
#include "BSML/Parsing/BSMLNodeParser.hpp"
#include "logging.hpp"

#if MAKE_DOCS
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/document.h"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/prettywriter.h"
#include "BSML/TypeHandlers/TypeHandler.hpp"
#include "BSML/Macros/BSMLMacro.hpp"
#include "BSML/Tags/BSMLTag.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "UnityEngine/GameObject.hpp"

#endif

namespace BSML {
    BSMLDocParser::TagNameToParser __attribute__((init_priority(200))) BSMLDocParser::tagNameToParser;
    BSMLNodeParserBase* defaultParser = nullptr;

    void BSMLDocParser::RegisterTag(BSMLNodeParserBase* parser) {
        for (auto& alias : parser->aliases) {
            DEBUG("Registering parser {} for tag {}", fmt::ptr(parser), alias);
            tagNameToParser.emplace(alias, parser);

            // Get the default bsml parser as a default parser, shortening lookup times later
            if (!defaultParser && alias == "bsml") {
                defaultParser = parser;
            }
        }

    }

    void BSMLDocParser::UnRegisterTag(BSMLNodeParserBase* parser) {
        for (auto& alias : parser->aliases) {
            auto itr = tagNameToParser.find(alias);
            if (itr != tagNameToParser.end())
                tagNameToParser.erase(itr);
        }
    }

    BSMLNodeParserBase* BSMLDocParser::get_parser(std::string alias) {
        auto itr = tagNameToParser.find(alias);
        if (itr == tagNameToParser.end())
            return defaultParser;
        return itr->second;
    }

    #if MAKE_DOCS
    void BSMLDocParser::PrintDocs() {
        std::vector<BSMLNodeParserBase*> parsers;
        // all unique parsers
        for (auto& [key, parser] : tagNameToParser) {
            if (std::find(parsers.begin(), parsers.end(), parser) == parsers.end()) parsers.push_back(parser);
        }

        auto& typeHandlers = TypeHandlerBase::get_typeHandlers();
        rapidjson::Document doc;
        doc.SetObject();
        auto& allocator = doc.GetAllocator();

        rapidjson::Value tags;
        tags.SetArray();
        rapidjson::Value macros;
        macros.SetArray();
        rapidjson::Value typehandlers;
        typehandlers.SetArray();

        auto parentObject = UnityEngine::GameObject::New_ctor("BSMLDocParent");
        auto t = parentObject->get_transform();

        // we should check every tag available
        for (auto parser : parsers) {
            // automatic cleanup if we forget
            std::unique_ptr<BSMLNode> node{parser->newNode()};
            const auto& name = parser->aliases[0];
            INFO("parser: {}", name);
            switch (node->nodeType) {
                case BSMLNode::NodeType::Macro: {
                    INFO("macro");
                    auto macro = reinterpret_cast<BSMLMacro*>(node.get());
                    rapidjson::Value macroVal;
                    macroVal.SetObject();

                    auto& props = macro->get_cachedProps();
                    rapidjson::Value propArray;
                    propArray.SetArray();

                    for (const auto& [key, aliases] : props) {
                        INFO("key: {}", key);
                        rapidjson::Value prop;
                        prop.SetObject();

                        rapidjson::Value aliasArray;
                        aliasArray.SetArray();

                        for (const auto& alias : aliases) {
                            INFO("\tkey alias: {}", alias);
                            aliasArray.PushBack(rapidjson::Value(alias.c_str(), alias.size(), allocator), allocator);
                        }

                        prop.AddMember("key", rapidjson::Value(key.c_str(), key.size(), allocator), allocator);
                        prop.AddMember("aliases", aliasArray, allocator);
                        propArray.PushBack(prop, allocator);
                    }

                    rapidjson::Value aliases;
                    aliases.SetArray();

                    for (const auto& alias : parser->aliases) {
                        INFO("parser alias: {}", alias);
                        aliases.PushBack(rapidjson::Value(alias.c_str(), alias.size(), allocator), allocator);
                    }

                    macroVal.AddMember("name", rapidjson::Value(name.c_str(), name.size(), allocator), allocator);
                    macroVal.AddMember("aliases", aliases, allocator);
                    macroVal.AddMember("properties", propArray, allocator);

                    macros.PushBack(macroVal, allocator);
                    break;
                }
                case BSMLNode::NodeType::Tag: {
                    INFO("tag");
                    auto tag = reinterpret_cast<BSMLTag*>(node.get());
                    rapidjson::Value tagVal;
                    tagVal.SetObject();

                    // create the object and set it to inactive so it can't do anything
                    auto obj = tag->CreateObject(t);
                    obj->SetActive(false);

                    auto externalComponents = obj->GetComponent<ExternalComponents*>();

                    // get all the typehandler type names that apply to this tag, we'll use it as a lookup system later
                    rapidjson::Value applicableTypeNames;
                    applicableTypeNames.SetArray();
                    for (auto handler : typeHandlers) {
                        auto type = handler->get_type();
                        if (!type) continue;
                        UnityEngine::Component* component = nullptr;
                        if (externalComponents) component = externalComponents->GetByType(type);
                        if (!component) component = obj->GetComponent(type);
                        if (!component) continue;
                        std::string typeName = type->FullNameOrDefault;
                        INFO("typehandler: {}", typeName);
                        applicableTypeNames.PushBack(rapidjson::Value(typeName.c_str(), typeName.size(), allocator), allocator);
                    }

                    rapidjson::Value aliases;
                    aliases.SetArray();

                    for (const auto& alias : parser->aliases) {
                        INFO("parser alias: {}", alias);
                        aliases.PushBack(rapidjson::Value(alias.c_str(), alias.size(), allocator), allocator);
                    }

                    tagVal.AddMember("name", rapidjson::Value(name.c_str(), name.size(), allocator), allocator);
                    tagVal.AddMember("aliases", aliases, allocator);
                    tagVal.AddMember("components", applicableTypeNames, allocator);

                    tags.PushBack(tagVal, allocator);
                    break;
                }
                case BSMLNode::NodeType::None: {
                    ERROR("BSMLParser created a node with NodeType::None!");
                    break;
                }
                default: {
                    ERROR("BSMLParser created a node with unknown NodeType!");
                    break;
                }
            }
        }

        UnityEngine::Object::DestroyImmediate(parentObject);

        // add every type handler to a value
        for (const auto handler : typeHandlers) {
            handler->AddToArray(typehandlers, allocator);
        }

        doc.AddMember("tags", tags, allocator);
        doc.AddMember("macros", macros, allocator);
        doc.AddMember("components", typehandlers, allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        mkpath("/sdcard/ModData/com.beatgames.beatsaber/Mods/BSML/docs/");

        if (fileexists("/sdcard/ModData/com.beatgames.beatsaber/Mods/BSML/docs/docs.json")) deletefile("/sdcard/ModData/com.beatgames.beatsaber/Mods/BSML/docs/docs.json");
        writefile("/sdcard/ModData/com.beatgames.beatsaber/Mods/BSML/docs/docs.json", {buffer.GetString(), buffer.GetLength()});
    }
    #endif
}
