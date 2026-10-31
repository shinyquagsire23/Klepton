#pragma once

#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"
#include "../Exceptions.hpp"
#include <fmt/core.h>

namespace BeatSaver {
    template<typename SelfT, typename T>
    struct Serde;

    /// HACK: hacky way to check for vector-like things
    template<typename T>
    concept has_value_type = requires() {
        typename T::value_type;
    };

    template<typename SelfT, typename T>
    requires(!has_value_type<T> || std::is_same_v<T, std::string>)
    struct Serde<SelfT, T> {
        using VT = T;
        static void Serialize(SelfT const* self, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator, VT SelfT::*valueOffset, std::string const& key) {
            auto k = rapidjson::Value(key, allocator);

            rapidjson::Value v;
            v.Set<VT>(self->*valueOffset, allocator);

            json.AddMember(k, v, allocator);
        }

        static void SerializeOpt(SelfT const* self, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator, std::optional<VT> SelfT::*valueOffset, std::string const& key) {
            auto& value = self->*valueOffset;
            if (value.has_value()) {
                auto k = rapidjson::Value(key, allocator);
                rapidjson::Value v;
                v.Set<VT>(value.value(), allocator);
                json.AddMember(k, v, allocator);
            }
        }

        static void Deserialize(SelfT* self, rapidjson::Value const& json, VT SelfT::*valueOffset, std::string const& key) {
            auto itr = json.FindMember(key);
            if (itr == json.MemberEnd()) {
                throw BeatSaver::JsonException(
                    BeatSaver::ExceptionKind::NoMember,
                    fmt::format("member '{}' was not found!", key)
                );
            }

            auto& jvalue = itr->value;
            if (!jvalue.Is<VT>()) {
                throw BeatSaver::JsonException(
                    BeatSaver::ExceptionKind::WrongType,
                    fmt::format("member '{}' was the wrong type! (expected {})", key, typeid(VT).name())
                );
            }

            self->*valueOffset = jvalue.Get<VT>();
        }

        static void DeserializeDefault(SelfT* self, rapidjson::Value const& json, VT SelfT::*valueOffset, std::string const& key) {
            auto itr = json.FindMember(key);
            // we had a default, act like optional
            if (itr == json.MemberEnd()) return;

            auto& jvalue = itr->value;
            // we had a default, act like optional
            if (jvalue.IsNull()) return;

            if (!jvalue.Is<VT>()) {
                throw BeatSaver::JsonException(
                    BeatSaver::ExceptionKind::WrongType,
                    fmt::format("member '{}' was the wrong type! (expected {})", key, typeid(VT).name())
                );
            }

            self->*valueOffset = jvalue.Get<VT>();
        }

        static void DeserializeOpt(SelfT* self, rapidjson::Value const& json, std::optional<VT> SelfT::*valueOffset, std::string const& key) {
            auto itr = json.FindMember(key);
            // optional, so missing is allowed (no throw)
            if (itr == json.MemberEnd()) return;

            auto& jvalue = itr->value;
            // optional, so null is allowed (no throw)
            if (jvalue.IsNull()) return;

            if (!jvalue.Is<VT>()) {
                throw BeatSaver::JsonException(
                    BeatSaver::ExceptionKind::WrongType,
                    fmt::format("member '{}' was the wrong type! (expected {})", key, typeid(VT).name())
                );
            }

            self->*valueOffset = jvalue.Get<VT>();
        }
    };

    template<typename SelfT, typename T>
    requires(has_value_type<T> && !std::is_same_v<T, std::string>)
    struct Serde<SelfT, T> {
        using VT = typename T::value_type;
        static void Serialize(SelfT const* self, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator, T SelfT::*valueOffset, std::string const& key) {
            auto k = rapidjson::Value(key, allocator);
            rapidjson::Value v;
            v.SetArray();
            for (auto& val : self->*valueOffset) {
                rapidjson::Value arrV;
                arrV.Set<VT>(val, allocator);
                v.PushBack(arrV, allocator);
            }
            json.AddMember(k, v, allocator);
        }

        static void SerializeOpt(SelfT const* self, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator, std::optional<T> SelfT::*valueOffset, std::string const& key) {
            auto& value = self->*valueOffset;
            if (!value.has_value()) return;

            auto k = rapidjson::Value(key, allocator);

            rapidjson::Value v;
            v.SetArray();
            for (auto& val : value) {
                rapidjson::Value arrV;
                arrV.Set<VT>(val, allocator);
                v.PushBack(arrV, allocator);
            }

            json.AddMember(k, v, allocator);
        }

        static void Deserialize(SelfT* self, rapidjson::Value const& json, T SelfT::*valueOffset, std::string const& key) {
            auto itr = json.FindMember(key);
            if (itr == json.MemberEnd()) {
                throw BeatSaver::JsonException(
                    BeatSaver::ExceptionKind::NoMember,
                    fmt::format("member '{}' was not found!", key)
                );
            }

            auto& arrayValue = itr->value;
            if (arrayValue.Is<VT>()) { // if it's the type we are deserializing into an array, it might just be an optimized "array length == 1"
                T value;
                value.emplace_back(arrayValue.Get<VT>());
                self->*valueOffset = std::move(value);
                return;
            }

            if (!arrayValue.IsArray()) {
                throw BeatSaver::JsonException(
                    BeatSaver::ExceptionKind::WrongType,
                    fmt::format("member '{}' was the wrong type! (expected json Array)", key)
                );
            }

            T value;
            for (int idx = -1; auto& jvalue : arrayValue.GetArray()) {
                idx++;
                if (!jvalue.Is<VT>()) {
                    throw BeatSaver::JsonException(
                        BeatSaver::ExceptionKind::WrongType,
                        fmt::format("array '{}' item at index {} was the wrong type! (expected {})", key, idx, typeid(VT).name())
                    );
                }

                value.emplace_back(jvalue.Get<VT>());
            }

            self->*valueOffset = std::move(value);
        }

        static void DeserializeOpt(SelfT* self, rapidjson::Value const& json, std::optional<T> SelfT::*valueOffset, std::string const& key) {
            auto itr = json.FindMember(key);
            // optional, so missing is allowed (no throw)
            if (itr == json.MemberEnd()) return;

            auto& arrayValue = itr->value;
            // optional, so null is allowed (no throw)
            if (arrayValue.IsNull()) return;

            if (arrayValue.Is<VT>()) { // if it's the type we are deserializing into an array, it might just be an optimized "array length == 1"
                T value;
                value.emplace_back(arrayValue.Get<VT>());
                self->*valueOffset = std::move(value);
                return;
            }

            if (!arrayValue.IsArray()) {
                throw BeatSaver::JsonException(
                    BeatSaver::ExceptionKind::WrongType,
                    fmt::format("member '{}' was the wrong type! (expected json Array)", key)
                );
            }

            T value;
            for (int idx = -1; auto& jvalue : arrayValue.GetArray()) {
                idx++;
                if (!jvalue.Is<VT>()) {
                    throw BeatSaver::JsonException(
                        BeatSaver::ExceptionKind::WrongType,
                        fmt::format("array '{}' item at index {} was the wrong type! (expected {})", key, idx, typeid(VT).name())
                    );
                }

                value.emplace_back(jvalue.Get<VT>());
            }
            self->*valueOffset = std::move(value);
        }

    };
}
