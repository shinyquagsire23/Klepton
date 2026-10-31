#pragma once

#include "beatsaber-hook/shared/utils/typedefs.h"

/// This macro defines a backing field, a getter and a setter for a property.
/// The field is declared as _name, the getter as get_name and the setter as set_name.
/// you can use these as BSMValue in your bsml file
#define DECLARE_BSML_PROPERTY(type, name)                                                                               \
DECLARE_INSTANCE_FIELD(type, _##name);                                                                                  \
public:                                                                                                                 \
type get_##name();                                                                                                      \
___CREATE_INSTANCE_METHOD(get_##name, "get_" #name, METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_HIDE_BY_SIG, nullptr);   \
public:                                                                                                                 \
void set_##name(type value);                                                                                            \
___CREATE_INSTANCE_METHOD(set_##name, "set_" #name, METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_HIDE_BY_SIG, nullptr)

/// method to be used by BSML_OPTIONS_LIST to convert the given options into an on demand getter for options
template<typename T>
inline ::ListW<T> initializer_list_to_List(std::initializer_list<std::string> list) {
    auto result = ::ListW<T>::New();
    result->EnsureCapacity(list.size());
    if constexpr (std::is_pointer_v<T>) {
        for (auto& v : list) result->Add(reinterpret_cast<T>(::StringW(v).convert()));
    } else {
        for (auto& v : list) result->Add(T(v));
    }
    return result;
}

// to be used with the various lists in BSML, pass the options as a list of strings, e.g. BSML_OPTIONS_LIST_OBJECT(lyrics, "Never", "Gonna", "Give", "You", "Up");
// the id of the list is the same as what you use for options="listid" in BSML, make sure your IDs are valid c++ identifiers!
#define BSML_OPTIONS_LIST_OBJECT(choices_id, ...) \
public: \
::ListW<System::Object*> get_##choices_id() { return initializer_list_to_List<System::Object*>({__VA_ARGS__}); };\
___CREATE_INSTANCE_METHOD(get_ ##choices_id, "get_" #choices_id, METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_HIDE_BY_SIG, nullptr)

// same as BSML_OPTIONS_LIST_OBJECT but returns a List<StringW>* instead of a List<System::Object*>*
#define BSML_OPTIONS_LIST_STRING(choices_id, ...) \
public: \
::ListW<::StringW>* get_##choices_id() { return initializer_list_to_List<::StringW>({__VA_ARGS__}); };\
___CREATE_INSTANCE_METHOD(get_ ##choices_id, "get_" #choices_id, METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_HIDE_BY_SIG, nullptr)
