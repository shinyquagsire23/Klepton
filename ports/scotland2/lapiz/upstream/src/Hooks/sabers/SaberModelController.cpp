#include "utilities/hooking.hpp"

#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/Saber.hpp"
#include "UnityEngine/Transform.hpp"

const MethodInfo* minfoInSelfOrParents(const Il2CppClass* klass, const char* methodName, int argc) {
    if (!klass) return nullptr;
    auto minfo = il2cpp_functions::class_get_method_from_name(klass, methodName, argc);
    return minfo ? minfo : minfoInSelfOrParents(klass->parent, methodName, argc);
}

std::set<GlobalNamespace::SaberModelController*> currentlyInvoking;
MAKE_AUTO_HOOK_MATCH(SaberModelController_Init, &GlobalNamespace::SaberModelController::Init, void, GlobalNamespace::SaberModelController* self, ::UnityEngine::Transform* parent, ::GlobalNamespace::Saber* saber, UnityEngine::Color trailTintColor) {
    static auto saberModelControllerKlass = classof(GlobalNamespace::SaberModelController*);
    if (self->klass == saberModelControllerKlass || currentlyInvoking.find(self) != currentlyInvoking.end()) SaberModelController_Init(self, parent, saber, trailTintColor);
    else {
        auto minfo = minfoInSelfOrParents(self->klass, "InitOverride", 2);
        if (minfo) {
            currentlyInvoking.emplace(self);
            if (minfo->return_type->type == Il2CppTypeEnum::IL2CPP_TYPE_BOOLEAN) {
                if (il2cpp_utils::RunMethodRethrow<bool>(self, minfo, parent, saber)) {
                    SaberModelController_Init(self, parent, saber, trailTintColor);
                }
            } else {
                il2cpp_utils::RunMethod(self, minfo, parent, saber);
            }
            currentlyInvoking.erase(self);
        } else SaberModelController_Init(self, parent, saber, trailTintColor);
    }

}
