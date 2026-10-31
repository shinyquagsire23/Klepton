#pragma once

#include "../../_config.h"
#include "../../StringParseHelper.hpp"
#include "../ComponentTypeWithData.hpp"
#include "../Parsing/BSMLParserParams.hpp"
#include "UnityEngine/Component.hpp"
#include "System/Type.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include "logging.hpp"

#if MAKE_DOCS
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/document.h"
#endif

namespace BSML {
    class BSML_EXPORT TypeHandlerBase {
        public:
            TypeHandlerBase() { RegisterTypeHandler(this); }
            ~TypeHandlerBase() { UnRegisterTypeHandler(this); }
            using Aliases = std::vector<std::string>;
            using PropMap = std::map<std::string, Aliases>;
            const PropMap& get_cachedProps();

            virtual PropMap get_props() const = 0;
            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) = 0;
            virtual void HandleTypeAfterChildren(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) { }
            virtual void HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) { }

            virtual System::Type* get_type() = 0;
            static std::vector<TypeHandlerBase*>& get_typeHandlers();

            virtual int get_priority() const { return 100; }

            #if MAKE_DOCS
                /// @brief method to add the typehandler to a given json value as a member
                /// @param val the value to add to
                /// @param allocator the allocator for the doc
                void AddToArray(rapidjson::Value& val, rapidjson::Document::AllocatorType& allocator);

                /// @brief method to make a rapidjson value of the typehandler
                /// @param allocator the allocator for the doc
                /// @return a rapidjson value of the typehandler name + properties
                rapidjson::Value ToJson(rapidjson::Document::AllocatorType& allocator);
            #endif
        private:
            PropMap cachedProps;
            static void RegisterTypeHandler(TypeHandlerBase* typeHandler);
            static void UnRegisterTypeHandler(TypeHandlerBase* typeHandler);
    };

    template<typename T>
    requires(std::is_convertible_v<T, UnityEngine::Component*>)
    class BSML_EXPORT TypeHandler : public TypeHandlerBase {
        public:
            TypeHandler() : TypeHandlerBase() {};

            using Setter = std::function<void(T, const StringParseHelper&)>;
            using SetterMap = std::map<std::string, Setter>;
            using PropMap = TypeHandlerBase::PropMap;

            const SetterMap& get_cachedSetters() {
                if (cachedSetters.empty())
                    cachedSetters = get_setters();
                return cachedSetters;
            }

            virtual System::Type* get_type() override {
                if (cachedType) return cachedType;
                return (cachedType = csTypeOf(T));
            };

            virtual SetterMap get_setters() const = 0;

            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override {
                auto klass = classof(T);
                if (il2cpp_functions::class_is_assignable_from(klass, componentType.component->klass)) {
                    auto& cachedSetters = get_cachedSetters();
                    auto& cachedProps = get_cachedProps();
                    // for each key value pair
                    for (const auto &[key, value] : componentType.data) {
                        INFO("data pair: '{}', '{}'", key, value);
                        // try to find the setter
                        auto itr = cachedSetters.find(key);
                        if (itr != cachedSetters.end()) {
                            INFO("got a setter!");
                            // execute the setter!
                            itr->second(reinterpret_cast<T>(componentType.component), value);
                        }
                    }
                } else {
                    ERROR("klass {}::{} was not assignable from {}::{}", klass->namespaze, klass->name, componentType.component->klass->namespaze, componentType.component->klass->name);
                }
            }
        private:
            System::Type* cachedType = nullptr;
            SetterMap cachedSetters;
    };
}
