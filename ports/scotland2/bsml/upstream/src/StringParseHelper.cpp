#include "StringParseHelper.hpp"
#include "Helpers/utilities.hpp"
#include "logging.hpp"

// splits this view into a vector of views into the different parts
std::vector<std::string_view> StringParseHelper::split(char split) const {
    std::vector<std::string_view> parts;
    std::size_t start = 0, end;
    while((end = this->find(split, start)) != std::string::npos) {
        auto part = this->substr(start, end - start);
        start = end + 1;
        // if empty, skip
        if (part.empty()) continue;
        // if only whitespace, skip this part
        if (std::find_if(part.begin(), part.end(), [](auto c){ return !std::isspace(c); }) == part.end()) continue;

        parts.emplace_back(part);
    }

    parts.emplace_back(this->substr(start));
    return parts;
}
/// makes a string thats lowercase
std::string StringParseHelper::toLower() const {
    std::string ret{data(), size()};
    std::transform(ret.begin(), ret.end(), ret.begin(), ::tolower);
    return ret;
}

std::string StringParseHelper::toUpper() const {
    std::string ret{data(), size()};
    std::transform(ret.begin(), ret.end(), ret.begin(), ::toupper);
    return ret;
}

std::optional<bool> StringParseHelper::tryParseBool() const {
    auto lower = toLower();
    if (lower == "true")
        return true;
    if (lower == "false")
        return false;
    return std::nullopt;
}

std::optional<int> StringParseHelper::tryParseInt() const {
    const char* begin = data();
    char* end = nullptr;
    int result = strtol(begin, &end, 10);
    if (*begin == '\0') return std::nullopt;
    if (end == begin || *end != '\0') return std::nullopt;
    return result;
}
std::optional<float> StringParseHelper::tryParseFloat() const {
    auto d = tryParseDouble();
    return d.has_value() ? std::make_optional<float>(d.value()) : std::nullopt;
}

std::optional<double> StringParseHelper::tryParseDouble() const {
    const char* begin = data();
    char* end = nullptr;
    double result = strtod(begin, &end);
    if (*begin == '\0') return std::nullopt;
    if (end == begin || *end != '\0') return std::nullopt;
    return result;
}
std::optional<UnityEngine::Color> StringParseHelper::tryParseColor() const {
    return BSML::Utilities::ParseHTMLColorOpt(*this);
}
std::optional<UnityEngine::Color32> StringParseHelper::tryParseColor32() const {
    return BSML::Utilities::ParseHTMLColor32Opt(*this);
}
std::optional<UnityEngine::Vector2> StringParseHelper::tryParseVector2(float defaultValue) const {
    auto parts = split(' ');
    switch (parts.size()) {
        case 1: {
            auto v = tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector2{v, v};
        } break;
        case 2: {
            auto x = StringParseHelper(parts[0]).tryParseFloat().value_or(defaultValue);
            auto y = StringParseHelper(parts[1]).tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector2{x, y};
        } break;
        default: return std::nullopt;
    }
}
std::optional<UnityEngine::Vector3> StringParseHelper::tryParseVector3(float defaultValue) const {
    auto parts = split(' ');
    switch (parts.size()) {
        case 1: {
            auto v = tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector3{v, v, v};
        } break;
        case 2: {
            auto x = StringParseHelper(parts[0]).tryParseFloat().value_or(defaultValue);
            auto y = StringParseHelper(parts[1]).tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector3{x, y, defaultValue};
        } break;
        case 3: {
            auto x = StringParseHelper(parts[0]).tryParseFloat().value_or(defaultValue);
            auto y = StringParseHelper(parts[1]).tryParseFloat().value_or(defaultValue);
            auto z = StringParseHelper(parts[2]).tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector3{x, y, z};
        } break;
        default: return std::nullopt;
    }
}
std::optional<UnityEngine::Vector4> StringParseHelper::tryParseVector4(float defaultValue) const {
    auto parts = split(' ');
    switch (parts.size()) {
        case 1: {
            auto v = tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector4{v, v, v, v};
        } break;
        case 2: {
            auto x = StringParseHelper(parts[0]).tryParseFloat().value_or(defaultValue);
            auto y = StringParseHelper(parts[1]).tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector4{x, y, x, y};
        } break;
        case 3: {
            auto x = StringParseHelper(parts[0]).tryParseFloat().value_or(defaultValue);
            auto y = StringParseHelper(parts[1]).tryParseFloat().value_or(defaultValue);
            auto z = StringParseHelper(parts[2]).tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector4{x, y, z, defaultValue};
        } break;
        case 4: {
            auto x = StringParseHelper(parts[0]).tryParseFloat().value_or(defaultValue);
            auto y = StringParseHelper(parts[1]).tryParseFloat().value_or(defaultValue);
            auto z = StringParseHelper(parts[2]).tryParseFloat().value_or(defaultValue);
            auto w = StringParseHelper(parts[3]).tryParseFloat().value_or(defaultValue);
            return UnityEngine::Vector4{x, y, z, defaultValue};
        } break;
        default: return std::nullopt;
    }
}
const MethodInfo* minfo_from_name_in_parents(Il2CppClass* klass, const char* name, int argc) {
    if (!klass) return nullptr;
    auto minfo = il2cpp_functions::class_get_method_from_name(klass, name, argc);
    if (minfo) return minfo;
    return il2cpp_functions::class_get_method_from_name(klass->parent, name, argc);
}
const MethodInfo* StringParseHelper::asMethodInfo(System::Object* host, int argCount) const {
    return minfo_from_name_in_parents(host->klass, data(), argCount);
}
const MethodInfo* StringParseHelper::asSetter(System::Object* host) const {
    auto name = "set_" + this->operator std::string();
    return il2cpp_functions::class_get_method_from_name(host->klass, name.c_str(), 1);
}
const MethodInfo* StringParseHelper::asGetter(System::Object* host) const {
    auto name = "get_" + this->operator std::string();
    return il2cpp_functions::class_get_method_from_name(host->klass, name.c_str(), 0);
}
FieldInfo* StringParseHelper::asFieldInfo(System::Object* host) const {
    return il2cpp_functions::class_get_field_from_name(host->klass, data());
}
StringParseHelper::operator StringW() const {
    return StringW(*this);
}
StringParseHelper::operator std::string() const {
    return {data(), size()};
}

StringParseHelper::operator bool() const {
    auto result = tryParseBool();
    if (!result.has_value()) {
        ERROR("Could not parse bool from input '{}'", *this);
        return false;
    }
    return result.value();
}

StringParseHelper::operator int() const {
    auto result = tryParseInt();
    if (!result.has_value()) {
        ERROR("Could not parse int from input '{}'", *this);
        return 0;
    }
    return result.value();
}
StringParseHelper::operator float() const {
    auto result = tryParseFloat();
    if (!result.has_value()) {
        ERROR("Could not parse float from input '{}'", *this);
        return 0;
    }
    return result.value();
}
StringParseHelper::operator double() const {
    auto result = tryParseDouble();
    if (!result.has_value()) {
        ERROR("Could not parse double from input '{}'", *this);
        return 0;
    }
    return result.value();
}
StringParseHelper::operator UnityEngine::Color() const {
    auto result = tryParseColor();
    if (!result.has_value()) {
        ERROR("Could not parse color from input '{}'", *this);
        return {1.0, 1.0, 1.0, 1.0};
    }
    return result.value();
}
StringParseHelper::operator UnityEngine::Color32() const {
    auto result = tryParseColor32();
    if (!result.has_value()) {
        ERROR("Could not parse color from input '{}'", *this);
        return {0, 255, 255, 255, 255};
    }
    return result.value();
}
StringParseHelper::operator UnityEngine::Vector2() const {
    auto result = tryParseVector2();
    if (!result.has_value()) {
        ERROR("Could not parse Vector2 from input '{}'", *this);
        return {0, 0};
    }
    return result.value();
}
StringParseHelper::operator UnityEngine::Vector3() const {
    auto result = tryParseVector3();
    if (!result.has_value()) {
        ERROR("Could not parse Vector3 from input '{}'", *this);
        return {0, 0, 0};
    }
    return result.value();
}
StringParseHelper::operator UnityEngine::Vector4() const {
    auto result = tryParseVector4();
    if (!result.has_value()) {
        ERROR("Could not parse Vector4 from input '{}'", *this);
        return {0, 0, 0, 0};
    }
    return result.value();
}
