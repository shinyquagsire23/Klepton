#include "utilities/hooking.hpp"
#include "arrayutils.hpp"

#include "sabers/effects/LapizSaberClashChecker.hpp"
#include "GlobalNamespace/SaberClashChecker.hpp"
#include "UnityEngine/Vector3.hpp"

#include "GlobalNamespace/GameplayCoreInstaller.hpp"
#include "GlobalNamespace/TutorialInstaller.hpp"

#include "Zenject/DiContainer.hpp"
#include "Zenject/ConcreteIdBinderGeneric_1.hpp"
#include "Zenject/BindStatement.hpp"
#include "Zenject/FromBinderNonGeneric.hpp"

MAKE_AUTO_HOOK_ORIG_MATCH(SaberClashChecker_AreSabersClashing, &GlobalNamespace::SaberClashChecker::AreSabersClashing, bool, GlobalNamespace::SaberClashChecker* self, ByRef<::UnityEngine::Vector3> clashingPoint) {
    static auto lapizCheckerKlass = classof(Lapiz::Sabers::Effects::LapizSaberClashChecker*);
    // if this object is of our own klass, so we can override the method call
    if (self->klass == lapizCheckerKlass) {
        auto customChecker = reinterpret_cast<Lapiz::Sabers::Effects::LapizSaberClashChecker*>(self);
        bool result = false;
        if (customChecker->SaberClashChecker_AreSabersClashing_override(clashingPoint.heldRef, result)) 
            result = SaberClashChecker_AreSabersClashing(self, clashingPoint);
        return result;
    } else {
        return SaberClashChecker_AreSabersClashing(self, clashingPoint);
    }
}

MAKE_AUTO_HOOK_MATCH(GameplayCoreInstaller_InstallBindings, &GlobalNamespace::GameplayCoreInstaller::InstallBindings, void, GlobalNamespace::GameplayCoreInstaller* self) {
    GameplayCoreInstaller_InstallBindings(self);
    auto container = self->get_Container();
    container->Unbind<GlobalNamespace::SaberClashChecker*>();
    container->Bind<GlobalNamespace::SaberClashChecker*>()->To(Lapiz::ArrayUtils::TypeArray<Lapiz::Sabers::Effects::LapizSaberClashChecker*>())->AsSingle();
}

MAKE_AUTO_HOOK_MATCH(TutorialInstaller_InstallBindings, &GlobalNamespace::TutorialInstaller::InstallBindings, void, GlobalNamespace::TutorialInstaller* self) {
    TutorialInstaller_InstallBindings(self);
    auto container = self->get_Container();
    container->Unbind<GlobalNamespace::SaberClashChecker*>();
    container->Bind<GlobalNamespace::SaberClashChecker*>()->To(Lapiz::ArrayUtils::TypeArray<Lapiz::Sabers::Effects::LapizSaberClashChecker*>())->AsSingle();
}
