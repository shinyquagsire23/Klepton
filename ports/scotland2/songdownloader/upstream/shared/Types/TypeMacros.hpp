#pragma once
#include "rapidjson-macros/shared/macros.hpp"

#include "../Exceptions.hpp"

#define GETTER_VALUE(type, name, jsonName) \
private: \
    NAMED_VALUE(type, _##name, jsonName); \
public: \
    const type& Get##name() const { return _##name; }

#define GETTER_VALUE_DEFAULT(type, name, def, jsonName) \
private: \
    NAMED_VALUE_DEFAULT(type, _##name, def, jsonName); \
public: \
    const type& Get##name() const { return _##name; }

#define GETTER_VALUE_OPTIONAL(type, name, jsonName) \
private: \
    NAMED_VALUE_OPTIONAL(type, _##name, jsonName); \
public: \
    std::optional<type> Get##name() const { return _##name; }

#define GETTER_CLASS_OPTIONAL(type, name, jsonName) \
private: \
    NAMED_VALUE_OPTIONAL(type, _##name, jsonName); \
public: \
    std::optional<type> Get##name() const { return _##name; }

#define ERROR_CHECK \
DESERIALIZE_FUNCTION(ErrorCheck) { \
    if (jsonValue.HasMember("error") && jsonValue["error"].IsString()) \
        throw SongDownloader::JsonException(SongDownloader::Exceptions::SiteError, jsonValue["error"].GetString()); \
}

// #define DESERIALIZE_VALUE_2TYPES(name, jsonName, returnType, checkType2) \
// if (!jsonValue.HasMember(#jsonName)) throw SongDownloader::JsonException(SongDownloader::Exceptions::NoMember, #jsonName); \
// if (jsonValue[#jsonName].Is##returnType() || jsonValue[#jsonName].Is##checkType2()) name = jsonValue[#jsonName].Get##returnType(); \
// else throw SongDownloader::JsonException(SongDownloader::Exceptions::WrongType, #jsonName ", Type expected was: " #returnType "or" #checkType2);
