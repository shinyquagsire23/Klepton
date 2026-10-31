#pragma once

#include "./_config.h"
#include <string>
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Color32.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Vector4.hpp"

struct BSML_EXPORT StringParseHelper : std::string_view {
    // pass the normal constructors through to the base, these are the only ones we need
    StringParseHelper(const std::string_view& str) : std::string_view(str) {}
    StringParseHelper(const char* str) : std::string_view(str) {}
    StringParseHelper(const char* str, size_type len) : std::string_view(str, len) {}
    StringParseHelper(const std::string& str) : std::string_view(str.c_str(), str.size()) {}

    /* -- operators -- */
    /// @brief string as C# string
    /// @return StringW
    operator StringW() const;

    /// @brief string as string
    /// @return std::string
    operator std::string() const;

    /// @brief string as bool
    /// @return bool or false if invalid bool string
    operator bool() const;

    /// @brief string as int
    /// @return int or 0 if invalid int string
    operator int() const;

    /// @brief string as float
    /// @return float or 0 if invalid float string
    operator float() const;

    /// @brief string as color
    /// @return double or 0 if invalid double string
    operator double() const;

    /// @return Color or white if invalid color string
    operator UnityEngine::Color() const;

    /// @brief string as color32
    /// @return Color32 or white if invalid color string
    operator UnityEngine::Color32() const;

    /// @brief string as vector2
    /// @return Vector2 or 0, 0 if invalid vector2 string
    operator UnityEngine::Vector2() const;

    /// @brief string as vector3
    /// @return Vector3 or 0, 0, 0 if invalid vector3 string
    operator UnityEngine::Vector3() const;

    /// @brief string as vector4
    /// @return Vector4 or 0, 0, 0, 0 if invalid vector4 string
    operator UnityEngine::Vector4() const;

    /* -- parsing methods -- */
    /// @brief try to parse a bool
    /// @return optional containing bool value, or nullopt if invalid bool string
    std::optional<bool> tryParseBool() const;

    /// @brief try to parse an int
    /// @return optional containing int value, or nullopt if invalid int string
    std::optional<int> tryParseInt() const;

    /// @brief try to parse a float
    /// @return optional containing float value, or nullopt if invalid float string
    std::optional<float> tryParseFloat() const;

    /// @brief try to parse a double
    /// @return optional containing double value, or nullopt if invalid double string
    std::optional<double> tryParseDouble() const;

    /// @brief try to parse a color
    /// @return optional containing color value, or nullopt if invalid color string
    std::optional<UnityEngine::Color> tryParseColor() const;

    /// @brief try to parse a color32
    /// @return optional containing color32 value, or nullopt if invalid color string
    std::optional<UnityEngine::Color32> tryParseColor32() const;

    /// @brief try to parse a vector2
    /// @return optional containing vector2 value, or nullopt if invalid vector2 string
    std::optional<UnityEngine::Vector2> tryParseVector2(float defaultValue = 0) const;

    /// @brief try to parse a vector3
    /// @return optional containing vector3 value, or nullopt if invalid vector2 string
    std::optional<UnityEngine::Vector3> tryParseVector3(float defaultValue = 0) const;

    /// @brief try to parse a vector4
    /// @return optional containing vector4 value, or nullopt if invalid vector2 string
    std::optional<UnityEngine::Vector4> tryParseVector4(float defaultValue = 0) const;

    /* -- reflection methods -- */
    /// @brief use the string for a method lookup in host->klass
    /// @param host the object in which' klass to look for a method
    /// @param argCount amount of args for the expected method
    /// @return method info for the method this string view would point to on host klass, nullptr if not found or wrong argCount
    const MethodInfo* asMethodInfo(System::Object* host, int argCount = 0) const;

    /// @brief use the string for a setter lookup in host->klass
    /// @param host the object in which' klass to look for a method
    /// @return method info for the setter this string view would point to on host klass, nullptr if not found
    const MethodInfo* asSetter(System::Object* host) const;

    /// @brief use the string for a getter lookup in host->klass
    /// @param host the object in which' klass to look for a method
    /// @return method info for the getter this string view would point to on host klass, nullptr if not found
    const MethodInfo* asGetter(System::Object* host) const;

    /// @brief use the string for a field lookup in host->klass
    /// @param host the object in which' klass to look for a field
    /// @return field info for the getter this string view would point to on host klass, nullptr if not found
    FieldInfo* asFieldInfo(System::Object* host) const;

    /// @brief a method to make the string view this helper refers to a lowercase string
    /// @return string of this helper, lowercase
    std::string toLower() const;

    /// @brief a method to make the string view this helper refers to an uppercase string
    /// @return string of this helper, uppercase
    std::string toUpper() const;

    /// @brief a method to split this string view into different views of the various parts of it
    /// @return vector of parts splitting this string view by split
    std::vector<std::string_view> split(char split) const;
};
