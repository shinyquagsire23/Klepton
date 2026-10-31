#pragma once

#include "web-utils/shared/Response.hpp"
#include "./Exceptions.hpp"
#include "./Models/Serde.hpp"

// declares a simple generic response that parses json into the set `type_`
#define BEATSAVER_PLUSPLUS_DECLARE_SIMPLE_RESPONSE_T(namespace_, type_)                            \
struct type_##Response : public WebUtils::GenericResponse<namespace_::type_> {  \
    bool AcceptData(std::span<uint8_t const> data) override {                   \
        rapidjson::Document doc;                                                \
        doc.Parse((char*)data.data(), data.size());                             \
        if (doc.HasParseError()) return false;                                  \
        try {                                                                   \
            responseData = doc.Get<namespace_::type_>();                        \
        } catch (BeatSaver::JsonException const& e) {                           \
            responseData = std::nullopt;                                        \
            return false;                                                       \
        }                                                                       \
        return true;                                                            \
    }                                                                           \
};

// performs a check for whether the given json contains a "error" member, and if so throws it as a site error
#define BEATSAVER_PLUSPLUS_ERROR_CHECK(json) do {                                                                                                  \
    auto itr = json.FindMember("error");                                                                                        \
    if (itr != json.MemberEnd()) throw BeatSaver::JsonException(BeatSaver::ExceptionKind::SiteError, itr->value.GetString());   \
} while(0)

// declares a type helper so rapidjson can perform v.Get<type_>();
#define BEATSAVER_PLUSPLUS_RAPIDJSON_TYPEHELPER(type_)                                                                                                                                 \
RAPIDJSON_NAMESPACE_BEGIN                                                                                                                                           \
namespace internal {                                                                                                                                                \
    template<typename ValueType>                                                                                                                                    \
    struct TypeHelper<ValueType, type_> {                                                                                                                           \
        static bool Is(const ValueType& v) { return v.IsObject(); }                                                                                                 \
        static type_ Get(const ValueType& v) { return type_::Deserialize(v); }                                                                                      \
        static ValueType& Set(ValueType& v, type_ data, typename ValueType::AllocatorType& allocator) { return type_::Serialize(data, v, allocator); }              \
    };                                                                                                                                                              \
}                                                                                                                                                                   \
RAPIDJSON_NAMESPACE_END

#define BEATSAVER_PLUSPLUS_SERDE_SETUP(type_)                                                                                                              \
    using SelfType = type_;                                                                                                             \
    static inline std::vector<std::function<void(SelfType const*, rapidjson::Value&, rapidjson::Value::AllocatorType&)>> serializers;   \
    static inline std::vector<std::function<void(SelfType*, rapidjson::Value const&)>> deserializers;                                   \
    static type_ Deserialize(rapidjson::Value const& json) {                                                                            \
        BEATSAVER_PLUSPLUS_ERROR_CHECK(json);                                                                                                              \
        type_ inst;                                                                                                                     \
        for (auto d : deserializers) d(&inst, json);                                                                                    \
        return inst;                                                                                                                    \
    }                                                                                                                                   \
    static rapidjson::Value Serialize(type_ const& inst, rapidjson::Value::AllocatorType& allocator) {                                  \
        rapidjson::Value json;                                                                                                          \
        for (auto s : serializers) s(&inst, json, allocator);                                                                           \
        return json;                                                                                                                    \
    }                                                                                                                                   \
    static rapidjson::Value& Serialize(type_ const& inst, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator) {         \
        for (auto s : serializers) s(&inst, json, allocator);                                                                           \
        return json;                                                                                                                    \
    }

#define SERDE_STRUCT(namespace_, name_, ...)    \
namespace namespace_ {                          \
    struct name_ {                              \
        BEATSAVER_PLUSPLUS_SERDE_SETUP(name_)                      \
        __VA_ARGS__                             \
    };                                          \
}                                               \
BEATSAVER_PLUSPLUS_RAPIDJSON_TYPEHELPER(namespace_::name_)

#define BEATSAVER_PLUSPLUS_GETTER_FIELD(type_, name_, key_)                                                                                            \
private:                                                                                                                            \
    type_ __##name_;                                                                                                                \
    __attribute__((constructor)) void __serde_##name_() {                                                                           \
        serializers.emplace_back([](SelfType const* self, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator) {     \
            return Serde<SelfType, type_>::Serialize(self, json, allocator, &SelfType::__##name_, key_);                            \
        });                                                                                                                         \
        deserializers.emplace_back([](SelfType* self, rapidjson::Value const& json) {                                               \
            return Serde<SelfType, type_>::Deserialize(self, json, &SelfType::__##name_, key_);                                     \
        });                                                                                                                         \
    }                                                                                                                               \
public:                                                                                                                             \
    type_ Get##name_() const { return __##name_; }  	                                                                            \
    __declspec(property(get=Get##name_)) type_ name_;

#define BEATSAVER_PLUSPLUS_GETTER_FIELD_DEFAULT(type_, name_, default_, key_)                                                                          \
private:                                                                                                                            \
    type_ __##name_ = default_;                                                                                                     \
    __attribute__((constructor)) void __serde_##name_() {                                                                           \
        serializers.emplace_back([](SelfType const* self, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator) {     \
            return Serde<SelfType, type_>::Serialize(self, json, allocator, &SelfType::__##name_, key_);                            \
        });                                                                                                                         \
        deserializers.emplace_back([](SelfType* self, rapidjson::Value const& json) {                                               \
            return Serde<SelfType, type_>::DeserializeDefault(self, json, &SelfType::__##name_, key_);                              \
        });                                                                                                                         \
    }                                                                                                                               \
public:                                                                                                                             \
    type_ Get##name_() const { return __##name_; }                                                                                  \
    __declspec(property(get=Get##name_)) type_ name_;

#define BEATSAVER_PLUSPLUS_GETTER_FIELD_OPTIONAL(type_, name_, key_)                                                                                   \
private:                                                                                                                            \
    std::optional<type_> __##name_;                                                                                                 \
    __attribute__((constructor)) void __serde_##name_() {                                                                           \
        serializers.emplace_back([](SelfType const* self, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator) {     \
            return Serde<SelfType, type_>::SerializeOpt(self, json, allocator, &SelfType::__##name_, key_);                         \
        });                                                                                                                         \
        deserializers.emplace_back([](SelfType* self, rapidjson::Value const& json) {                                               \
            return Serde<SelfType, type_>::DeserializeOpt(self, json, &SelfType::__##name_, key_);                                  \
        });                                                                                                                         \
    }                                                                                                                               \
public:                                                                                                                             \
    std::optional<type_> Get##name_() const { return __##name_; }                                                                   \
    __declspec(property(get=Get##name_)) type_ name_;

#define BEATSAVER_PLUSPLUS_RESPONSE_STRUCT(namespace_, name_, __VA_ARGS__) \
SERDE_STRUCT(namespace_, name_, __VA_ARGS__);           \
namespace namespace_ {                                  \
    BEATSAVER_PLUSPLUS_DECLARE_SIMPLE_RESPONSE_T(namespace_, name_);       \
}                                                       \
