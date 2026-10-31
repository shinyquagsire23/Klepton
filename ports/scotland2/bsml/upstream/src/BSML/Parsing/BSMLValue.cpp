#include "BSML/Parsing/BSMLValue.hpp"

void collect_finfos(Il2CppClass* klass, std::vector<FieldInfo*>& finfos) {
    if (!klass) return;
    void* iter = nullptr;
    FieldInfo* finfo = nullptr;
    while ((finfo = il2cpp_functions::class_get_fields(klass, &iter))) finfos.push_back(finfo);
    collect_finfos(il2cpp_functions::class_get_parent(klass), finfos);
}

void collect_pinfos(Il2CppClass* klass, std::vector<const PropertyInfo*>& pinfos) {
    if (!klass) return;
    void* iter = nullptr;
    const PropertyInfo* pinfo = nullptr;
    while ((pinfo = il2cpp_functions::class_get_properties(klass, &iter))) pinfos.push_back(pinfo);
    collect_pinfos(il2cpp_functions::class_get_parent(klass), pinfos);
}

void collect_minfos(Il2CppClass* klass, std::vector<const MethodInfo*>& minfos) {
    if (!klass) return;
    void* iter = nullptr;
    const MethodInfo* minfo = nullptr;
    while ((minfo = il2cpp_functions::class_get_methods(klass, &iter))) minfos.push_back(minfo);
    collect_minfos(il2cpp_functions::class_get_parent(klass), minfos);
}

namespace BSML {
    std::map<std::string, BSMLValue*> BSMLValue::MakeValues(System::Object* host) {
        std::vector<FieldInfo*> finfos{};
        std::vector<const PropertyInfo*> pinfos{};
        std::vector<const MethodInfo*> minfos{};

        // collect fields, props and methods
        collect_finfos(il2cpp_functions::object_get_class(host), finfos);
        collect_pinfos(il2cpp_functions::object_get_class(host), pinfos);
        collect_minfos(il2cpp_functions::object_get_class(host), minfos);

        std::map<std::string, BSMLValue*> values{};
        // every field is a new Value
        for (auto finfo : finfos) {
            auto val = new BSMLValue();
            val->name = finfo->name;
            val->host = host;
            val->fieldInfo = finfo;

            values.emplace(finfo->name, val);
        }

        // properties can double for fields
        for (auto pinfo : pinfos) {
            auto itr = values.find(pinfo->name);
            if (itr != values.end()) { // if the value existed
                auto val = itr->second;
                val->setterInfo = il2cpp_functions::property_get_set_method(pinfo);
                val->getterInfo = il2cpp_functions::property_get_set_method(pinfo);
            } else { // if the value did not yet exist
                auto val = new BSMLValue();
                val->host = host;
                val->name = pinfo->name;
                val->setterInfo = il2cpp_functions::property_get_set_method(pinfo);
                val->getterInfo = il2cpp_functions::property_get_set_method(pinfo);
                values.emplace(pinfo->name, val);
            }
        }

        // CT "props" are just methods so we gotta check those too
        for (auto minfo : minfos) {
            std::string_view name = minfo->name;
            bool set = name.starts_with("set_");
            bool get = name.starts_with("get_");
            if (!set && !get) continue;
            std::string rest{name.substr(4, name.size() - 4)};
            auto itr = values.find(rest);
            if (itr != values.end()) { // if the value existed
                auto val = itr->second;
                if (set) val->setterInfo = minfo;
                else if (get) val->getterInfo = minfo;
            } else { // if it did not exist
                auto val = new BSMLValue();
                val->host = host;
                val->name = rest;
                if (set) val->setterInfo = minfo;
                else if (get) val->getterInfo = minfo;
                values.emplace(rest, val);
            }
        }

        return values;
    }

    void BSMLValue::SetValue(System::Object* val) {
        if (fieldInfo) {
            il2cpp_functions::field_set_value(host, fieldInfo, &val);
        } else if (setterInfo) {
            il2cpp_utils::RunMethod(host, setterInfo, val);
        }
    }

    System::Object* BSMLValue::GetValue() {
        if (fieldInfo) {
            System::Object* val;
            il2cpp_functions::field_get_value(host, fieldInfo, &val);
            return val;
        } else if (getterInfo) {
            return il2cpp_utils::RunMethodOpt<System::Object*>(host, getterInfo).value_or(nullptr);
        }
        return nullptr;
    }
}
