// Host-only guest fixture. Never included in the shipping mod inventory.
#include "custom-types/shared/macros.hpp"
#include "custom-types/shared/register.hpp"
#include "beatsaber-hook/shared/api.hpp"
#include "System/Object.hpp"
#include <cstdio>
#include <cstdint>

DECLARE_CLASS_CODEGEN(Scotland2PortTest, RegistrationProbe, System::Object) {
    DECLARE_INSTANCE_FIELD(int, value);
    DECLARE_CTOR(ctor);
};
DEFINE_TYPE(Scotland2PortTest, RegistrationProbe);
void Scotland2PortTest::RegistrationProbe::ctor() { value = 1453; }

extern "C" __attribute__((visibility("default")))
int sc2_test_1453_sdk(void** targets, size_t capacity) {
    if (!targets || capacity != 3) return 1;
    using namespace i2c::functions;
    initialize();
    CheckS_GlobalMetadata();
    if (!s_GlobalMetadata || !s_GlobalMetadataHeader || !s_Il2CppMetadataRegistration ||
        static_cast<uint32_t>(s_GlobalMetadataHeader->sanity) != 0xFAB11BAF ||
        s_GlobalMetadataHeader->version != 39) return 2;
    targets[0] = reinterpret_cast<void*>(Class_FromIl2CppTypeNative);
    targets[1] = reinterpret_cast<void*>(Class_InitNative);
    targets[2] = reinterpret_cast<void*>(GlobalMetadata_GetTypeInfoFromTypeDefinitionIndex);
    if (!GlobalMetadata_GetTypeInfoFromHandle || !MetadataCache_GetTypeInfoFromHandle) return 3;
    auto count = s_Il2CppMetadataRegistration->typeDefinitionsSizesCount;
    if (count <= 0) return 4;
    for (TypeDefinitionIndex index : {0, count - 1}) {
        auto klass = GlobalMetadata_GetTypeInfoFromTypeDefinitionIndex(index);
        if (!klass || type_get_class(const_cast<Il2CppType*>(class_get_type_const(klass))) != klass) return 5;
        if (GlobalMetadata_GetTypeInfoFromHandle(klass->typeMetadataHandle) != klass ||
            MetadataCache_GetTypeInfoFromHandle(klass->typeMetadataHandle) != klass) return 6;
    }
    // Custom-types deliberately uses negative definition indices for its own
    // classes. The unhooked game's invalid sentinel test applies before that
    // registry is installed; after early loading it denotes custom class zero.
    if (custom_types::Register::getTypes().empty() &&
        GlobalMetadata_GetTypeInfoFromTypeDefinitionIndex(kTypeDefinitionIndexInvalid)) return 7;
    return 0;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_1453_registration() {
    using namespace i2c::functions;
    custom_types::Register::AutoRegister();
    const auto count = custom_types::Register::getTypes().size();
    if (!count) return 1;
    Il2CppClass* probe = nullptr;
    for (auto type : custom_types::Register::getTypes()) {
        auto klass = type->klass();
        if (!klass || !type->initialized()) return 2;
        if (class_from_il2cpp_type(&klass->byval_arg) != klass) return 3;
        if (class_from_name(klass->image, klass->namespaze, klass->name) != klass) return 4;
        if (std::string_view(klass->namespaze) == "Scotland2PortTest" &&
            std::string_view(klass->name) == "RegistrationProbe") probe = klass;
    }
    if (!probe) return 5;
    auto object = object_new(probe);
    if (!object || object->klass != probe) return 6;
    runtime_object_init(object);
    auto field = class_get_field_from_name(probe, "value");
    if (!field) return 7;
    int value = 0;
    field_get_value(object, field, &value);
    if (value != 1453) return 8;
    custom_types::Register::AutoRegister();
    if (custom_types::Register::getTypes().size() != count) return 9;
    printf("CUSTOM_TYPES_1453_OK %zu registered types including the diagnostic type; object constructor/field and repeated registration passed.\n", count);
    return 0;
}
