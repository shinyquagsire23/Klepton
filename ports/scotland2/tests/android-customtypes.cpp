#define IL2CPP_FUNC_VISIBILITY public
#include "register.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include <cstdint>
#include "beatsaber-hook/shared/utils/hook-tracker.hpp"
extern "C" __attribute__((visibility("default")))
const void* sc2_test_hook_previous(const void* destination,const void* replacement) {
    auto hooks=HookTracker::GetHooks();
    auto at=hooks->find(destination);
    if(at==hooks->end())return nullptr;
    for(const auto& info:at->second)
        if(info.trampoline==replacement)return info.orig;
    return nullptr;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_customtypes_register() {
    custom_types::Register::AutoRegister();
    auto count=custom_types::Register::getTypes().size();
    if (count!=2) return 1;
    for (auto type:custom_types::Register::getTypes()) {
        Il2CppClass *klass=type->klass();
        if (!klass || !type->initialized() || !klass->method_count || !klass->field_count) return 2;
        if (il2cpp_functions::class_from_il2cpp_type(&klass->byval_arg)!=klass) return 3;
        if (il2cpp_functions::class_from_name(klass->image,klass->namespaze,klass->name)!=klass) return 4;
        auto *object=il2cpp_functions::object_new(klass);
        if (!object || object->klass!=klass) return 5;
    }
    custom_types::Register::AutoRegister();
    if (custom_types::Register::getTypes().size()!=count) return 6;
    return 0;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_lapiz_types() {
    const auto count=custom_types::Register::getTypes().size();
    // Lapiz also instantiates two original custom-types delegate wrappers.
    if(count!=24) return 1;
    unsigned lapiz=0;Il2CppClass *scheduler=nullptr;
    for(auto type:custom_types::Register::getTypes()) {
        auto klass=type->klass();
        if(!klass || !type->initialized())return 2;
        if(il2cpp_functions::class_from_il2cpp_type(&klass->byval_arg)!=klass)return 3;
        if(il2cpp_functions::class_from_name(klass->image,klass->namespaze,klass->name)!=klass)return 4;
        if(std::string_view(klass->namespaze).starts_with("Lapiz"))++lapiz;
        if(std::string_view(klass->namespaze)=="Lapiz::Utilities" && std::string_view(klass->name)=="MainThreadScheduler")scheduler=klass;
    }
    if(lapiz!=20 || !scheduler)return 5;
    auto object=il2cpp_functions::object_new(scheduler);
    if(!object || object->klass!=scheduler)return 6;
    auto tick=il2cpp_functions::class_get_method_from_name(scheduler,"Tick",0);
    if(!tick || !tick->methodPointer)return 7;
    // This original Lapiz method checks its empty queue; no Unity startup needed.
    reinterpret_cast<void(*)(Il2CppObject*,const MethodInfo*)>(tick->methodPointer)(object,tick);
    custom_types::Register::AutoRegister();
    return custom_types::Register::getTypes().size()==count?0:8;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_bsml_types() {
    const auto count=custom_types::Register::getTypes().size();
    unsigned bsml=0;
    for(auto type:custom_types::Register::getTypes()) {
        auto klass=type->klass();
        if(!klass || !type->initialized())return 1;
        if(il2cpp_functions::class_from_il2cpp_type(&klass->byval_arg)!=klass)return 2;
        if(il2cpp_functions::class_from_name(klass->image,klass->namespaze,klass->name)!=klass)return 3;
        if(std::string_view(klass->namespaze).starts_with("BSML"))++bsml;
    }
    if(bsml!=61)return 4;
    custom_types::Register::AutoRegister();
    return custom_types::Register::getTypes().size()==count?0:5;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_bshook_mkpath(const char *path,size_t size) {
    return ::mkpath(std::string_view(path,size));
}

extern "C" __attribute__((visibility("default")))
int sc2_test_songcore_types() {
    custom_types::Register::AutoRegister();
    const auto count=custom_types::Register::getTypes().size();
    unsigned songcore=0;
    for(auto type:custom_types::Register::getTypes()) {
        auto klass=type->klass();
        if(!klass || !type->initialized())return 1;
        if(il2cpp_functions::class_from_il2cpp_type(&klass->byval_arg)!=klass)return 2;
        if(il2cpp_functions::class_from_name(klass->image,klass->namespaze,klass->name)!=klass)return 3;
        if(std::string_view(klass->namespaze).starts_with("SongCore"))++songcore;
    }
    if(songcore!=23)return 4;
    custom_types::Register::AutoRegister();
    return custom_types::Register::getTypes().size()==count?0:5;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_metacore_types() {
    custom_types::Register::AutoRegister();
    const auto count=custom_types::Register::getTypes().size();
    unsigned metacore=0;
    for(auto type:custom_types::Register::getTypes()) {
        auto klass=type->klass();
        if(!klass || !type->initialized())return 1;
        if(il2cpp_functions::class_from_il2cpp_type(&klass->byval_arg)!=klass)return 2;
        if(il2cpp_functions::class_from_name(klass->image,klass->namespaze,klass->name)!=klass)return 3;
        if(std::string_view(klass->namespaze).starts_with("MetaCore"))++metacore;
    }
    if(metacore!=4)return 4;
    custom_types::Register::AutoRegister();
    return custom_types::Register::getTypes().size()==count?0:5;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_playlistcore_types() {
    custom_types::Register::AutoRegister();
    const auto count=custom_types::Register::getTypes().size();
    unsigned playlistcore=0;
    for(auto type:custom_types::Register::getTypes()) {
        auto klass=type->klass();
        if(!klass || !type->initialized())return 1;
        if(il2cpp_functions::class_from_il2cpp_type(&klass->byval_arg)!=klass)return 2;
        if(il2cpp_functions::class_from_name(klass->image,klass->namespaze,klass->name)!=klass)return 3;
        if(std::string_view(klass->namespaze).starts_with("PlaylistCore"))++playlistcore;
    }
    if(playlistcore!=5)return 4;
    custom_types::Register::AutoRegister();
    return custom_types::Register::getTypes().size()==count?0:5;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_types() {
    custom_types::Register::AutoRegister();
    const auto count=custom_types::Register::getTypes().size();
    unsigned songdownloader=0;
    for(auto type:custom_types::Register::getTypes()) {
        auto klass=type->klass();
        if(!klass || !type->initialized())return 1;
        if(il2cpp_functions::class_from_il2cpp_type(&klass->byval_arg)!=klass)return 2;
        if(il2cpp_functions::class_from_name(klass->image,klass->namespaze,klass->name)!=klass)return 3;
        if(std::string_view(klass->namespaze).starts_with("SongDownloader"))++songdownloader;
    }
    if(songdownloader!=4)return 4;
    custom_types::Register::AutoRegister();
    return custom_types::Register::getTypes().size()==count?0:5;
}
