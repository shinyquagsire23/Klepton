#include "utilities/hooking.hpp"

#include "sabers/SaberModelProvider.hpp"
#include "GlobalNamespace/SetSaberGlowColor.hpp"
#include "GlobalNamespace/SetSaberFakeGlowColor.hpp"
#include "GlobalNamespace/SaberModelContainer.hpp"

MAKE_AUTO_HOOK_MATCH(SetSaberGlowColor_Start, &GlobalNamespace::SetSaberGlowColor::Start, void, GlobalNamespace::SetSaberGlowColor* self) {
    auto instance = Lapiz::Sabers::SaberModelProvider::get_instance();
    if (!instance || instance->SetSaberGlowColor_Start_Prefix(self))
        SetSaberGlowColor_Start(self);
}

MAKE_AUTO_HOOK_MATCH(SetSaberFakeGlowColor_Start, &GlobalNamespace::SetSaberFakeGlowColor::Start, void, GlobalNamespace::SetSaberFakeGlowColor* self) {
    auto instance = Lapiz::Sabers::SaberModelProvider::get_instance();
    if (!instance || instance->SetSaberFakeGlowColor_Start_Prefix(self))
        SetSaberFakeGlowColor_Start(self);
}

MAKE_AUTO_HOOK_MATCH(SaberModelContainer_Start, &GlobalNamespace::SaberModelContainer::Start, void, GlobalNamespace::SaberModelContainer* self) {
    auto instance = Lapiz::Sabers::SaberModelProvider::get_instance();
    if (instance) instance->SaberModelContainer_Start_Prefix(self);
    SaberModelContainer_Start(self);
}