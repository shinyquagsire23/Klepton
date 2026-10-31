#include "utilities/hooking.hpp"

#include "sabers/effects/SaberClashEffectAdjuster.hpp"

MAKE_AUTO_HOOK_MATCH(SaberClashEffect_Start, &GlobalNamespace::SaberClashEffect::Start, void, GlobalNamespace::SaberClashEffect* self) {
    SaberClashEffect_Start(self);
    auto instance = Lapiz::Sabers::Effects::SaberClashEffectAdjuster::get_instance();
    if (instance) {
        instance->SaberClashEffect_Start_Postfix(self);
    }
}
