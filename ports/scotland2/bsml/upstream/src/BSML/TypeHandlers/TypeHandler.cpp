#include "BSML/TypeHandlers/TypeHandler.hpp"
#include "Helpers/utilities.hpp"

namespace BSML {

    std::vector<TypeHandlerBase*>& TypeHandlerBase::get_typeHandlers() {
        static std::vector<TypeHandlerBase*> typeHandlers = {};
        return typeHandlers;
    }

    const TypeHandlerBase::PropMap& TypeHandlerBase::get_cachedProps() {
        if (cachedProps.empty())
            cachedProps = get_props();
        return cachedProps;
    }

    void TypeHandlerBase::RegisterTypeHandler(TypeHandlerBase* typeHandler) {
        auto& typeHandlers = get_typeHandlers();
        auto itr = std::find(typeHandlers.begin(), typeHandlers.end(), typeHandler);
        if (itr == typeHandlers.end()) {
            INFO("Registered type handler");
            typeHandlers.emplace_back(typeHandler);
        }
        std::sort(typeHandlers.begin(), typeHandlers.end(), [](auto lhs, auto rhs) { return lhs->get_priority() < rhs->get_priority(); });
        INFO("type handler count: {}", typeHandlers.size());
    }

    void TypeHandlerBase::UnRegisterTypeHandler(TypeHandlerBase* typeHandler) {
        auto& typeHandlers = get_typeHandlers();
        auto itr = std::find(typeHandlers.begin(), typeHandlers.end(), typeHandler);
        if (itr != typeHandlers.end()) {
            INFO("UnRegistered type handler");
            typeHandlers.erase(itr, itr++);
        }
        INFO("type handler count: {}", typeHandlers.size());
    }

    #if MAKE_DOCS
    void TypeHandlerBase::AddToArray(rapidjson::Value& val, rapidjson::Document::AllocatorType& allocator) {
        val.PushBack(ToJson(allocator), allocator);
    }

    rapidjson::Value TypeHandlerBase::ToJson(rapidjson::Document::AllocatorType& allocator) {
        rapidjson::Value typehandler;
        typehandler.SetObject();
        std::string name =  get_type()->FullNameOrDefault;
        typehandler.AddMember("typename", rapidjson::Value(name.c_str(), name.size(), allocator), allocator);

        rapidjson::Value propertiesArray;
        propertiesArray.SetArray();

        auto& properties = get_cachedProps();
        for (const auto& [key, aliases]  : properties) {
            rapidjson::Value prop;
            prop.SetObject();

            rapidjson::Value aliasArray;
            aliasArray.SetArray();
            for (const auto& alias : aliases) {
                aliasArray.PushBack(rapidjson::Value(alias.c_str(), alias.size(), allocator), allocator);
            }
            prop.AddMember("key", rapidjson::Value(key.c_str(), key.size(), allocator), allocator);
            prop.AddMember("aliases", aliasArray, allocator);
            propertiesArray.PushBack(prop, allocator);
        }

        typehandler.AddMember("properties", propertiesArray, allocator);
        return typehandler;
    }
    #endif
}
