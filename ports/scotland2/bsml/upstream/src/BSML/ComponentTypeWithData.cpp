#include "BSML/ComponentTypeWithData.hpp"
#include "logging.hpp"

std::string BSMLValueToString(BSML::BSMLValue* v, Il2CppTypeEnum type);

namespace BSML {
    std::map<std::string, std::string> ComponentTypeWithData::GetParameters(const std::map<std::string, std::string>& allParams, const BSMLParserParams& parserParams, const std::map<std::string, std::vector<std::string>>& props) {
        std::map<std::string, std::string> result;

        for (const auto& [prop, vec] : props) {
            for (const auto& alias : vec) {
                auto itr = allParams.find(alias);
                if (itr == allParams.end()) continue;
                if (!itr->second.empty() && itr->second[0] == '~') {
                    // if start with ~ look the value up in the parserParams values
                    auto key = itr->second.substr(1);
                    auto v = parserParams.TryGetValue(key);
                    if (v) {
                        if (v->fieldInfo) {
                            result[prop] = BSMLValueToString(v, v->fieldInfo->type->type);
                        } else if (v->getterInfo) {
                            result[prop] = BSMLValueToString(v, v->getterInfo->return_type->type);
                        }

                        continue;
                    } else {
                        ERROR("Could not find value for '{}'", key);
                    }
                    // if the value was not found we assign the actual name to the prop so it can at least try to be used
                }
                result[prop] = itr->second;
                break;
            }
        }

        return result;
    }
}

std::string BSMLValueToString(BSML::BSMLValue* v, Il2CppTypeEnum type) {
    switch (type) {
        case Il2CppTypeEnum::IL2CPP_TYPE_END: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_VOID:
            return "void";
        case Il2CppTypeEnum::IL2CPP_TYPE_BOOLEAN:
            return v->GetValue<bool>() ? "true" : "false";
        case Il2CppTypeEnum::IL2CPP_TYPE_CHAR: {
            char val[3] = "\0\0";
            *reinterpret_cast<Il2CppChar*>(val) = v->GetValue<Il2CppChar>();
            return fmt::format("{}", val);
        }
        case Il2CppTypeEnum::IL2CPP_TYPE_I1:
            return fmt::format("{}", v->GetValue<int8_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_U1:
            return fmt::format("{}", v->GetValue<uint8_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_I2:
            return fmt::format("{}", v->GetValue<int16_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_U2:
            return fmt::format("{}", v->GetValue<uint16_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_I4:
            return fmt::format("{}", v->GetValue<int32_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_U4:
            return fmt::format("{}", v->GetValue<uint32_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_I8:
            return fmt::format("{}", v->GetValue<int64_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_U8:
            return fmt::format("{}", v->GetValue<uint64_t>());
        case Il2CppTypeEnum::IL2CPP_TYPE_R4:
            return fmt::format("{}", v->GetValue<float>());
        case Il2CppTypeEnum::IL2CPP_TYPE_R8:
            return fmt::format("{}", v->GetValue<double>());
        case Il2CppTypeEnum::IL2CPP_TYPE_STRING:
            return v->GetValue<StringW>();
        case Il2CppTypeEnum::IL2CPP_TYPE_PTR: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_BYREF: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_VALUETYPE: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_VAR: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_ARRAY: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_GENERICINST: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_TYPEDBYREF:
            return "UNKNOWN TYPE VALUE";
        case Il2CppTypeEnum::IL2CPP_TYPE_I:
            return fmt::format("{}", v->GetValue<int>());
        case Il2CppTypeEnum::IL2CPP_TYPE_U:
            return fmt::format("{}", v->GetValue<uint>());
        case Il2CppTypeEnum::IL2CPP_TYPE_FNPTR:
            return "0";
        case Il2CppTypeEnum::IL2CPP_TYPE_CLASS: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_OBJECT: {
            auto obj = v->GetValue();
            if (!obj) return "NULL";
            return obj->ToString();
        }
        case Il2CppTypeEnum::IL2CPP_TYPE_SZARRAY: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_MVAR: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_CMOD_REQD: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_CMOD_OPT: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_INTERNAL: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_MODIFIER: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_SENTINEL: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_PINNED: [[fallthrough]];
        case Il2CppTypeEnum::IL2CPP_TYPE_ENUM: [[fallthrough]];
        default:
            return "UNKNOWN VALUE TYPE";
    }
}
